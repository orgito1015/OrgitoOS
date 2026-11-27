/*
 * OrgitoOS - exec(): load a real ELF32 binary into a new process.
 *
 * Reads an executable file from the VFS, checks its ELF header, creates a
 * new user process in its own address space and maps every PT_LOAD
 * segment into freshly allocated private pages: file bytes are copied in,
 * the rest (.bss) is zero-filled. The process then starts at the ELF
 * entry point in ring 3, on its own user stack.
 *
 * Design note - spawn, not in-place replace: POSIX exec() replaces the
 * calling process's image (same pid). Here exec creates a brand-new child
 * process instead (like posix_spawn()), which composes cleanly with the
 * existing process code; fork() deep-copies loaded images, so fork-then-
 * exec also works.
 *
 * Race-free start: the process is created SUSPENDED
 * (process_create_user_suspended) and only made runnable with
 * process_start() once every page is mapped - otherwise a timer tick could
 * schedule it before its code exists.
 *
 * Programs are built from userland/ (see the Makefile) as standalone ELF
 * files, embedded in the kernel image and copied into /bin at boot by
 * exec_install_embedded_programs().
 */

#include "exec.h"
#include "string.h"
#include "fileio.h"
#include "../drivers/console.h"
#include "../fs/vfs.h"
#include "../memory/heap.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"

/* ------------------------------------------------------------------ */
/* Minimal ELF32 structures                                             */
/* ------------------------------------------------------------------ */

#define EI_NIDENT   16
#define ELFMAG0     0x7F
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define ELFCLASS32  1
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define EM_386      3
#define PT_LOAD     1
#define PF_X        1
#define PF_W        2

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf32_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) elf32_phdr_t;

/* User images must live in this window: above the user stack and far from
 * the kernel's identity map (memory/vmm.c), below the top of memory. */
#define EXEC_USER_MIN 0x10000000u
#define EXEC_USER_MAX 0xC0000000u

#define EXEC_PAGE_ALIGN_DOWN(x) ((uint32_t)(x) & ~(PAGE_SIZE - 1u))
#define EXEC_PAGE_ALIGN_UP(x)   (((uint32_t)(x) + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u))

/* Basename of a path, for the process table's name[] field. */
static void basename_into(const char *path, char *out, size_t out_size) {
    const char *slash = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/') slash = p;
    }
    const char *base = slash ? slash + 1 : path;
    strncpy(out, base, out_size - 1);
    out[out_size - 1] = '\0';
    if (out[0] == '\0') {
        strncpy(out, "exec", out_size - 1);
    }
}

static void print_num(uint32_t v, int base) {
    char buf[12];
    itoa((int)v, buf, base);
    console_write(buf);
}

const char *exec_strerror(int err) {
    switch (err) {
        case EXEC_ENOENT:  return "No such file or directory";
        case EXEC_EISDIR:  return "Is a directory";
        case EXEC_EACCES:  return "Permission denied";
        case EXEC_ENOEXEC: return "Exec format error";
        case EXEC_ENOMEM:  return "Cannot allocate memory";
        default:           return "Unknown error";
    }
}

process_t *process_exec(const char *path, uint8_t priority, int verbose, int *err) {
    int dummy;
    if (!err) err = &dummy;
    *err = 0;

    vfs_node_t *file = fileio_lookup(path);
    if (!file) { *err = EXEC_ENOENT; return 0; }
    if (file->type != NODE_FILE) { *err = EXEC_EISDIR; return 0; }
    /* Execute permission (owner x bit), like Linux. */
    if (!(fileio_node_mode(file) & 0100)) { *err = EXEC_EACCES; return 0; }
    if (file->length < sizeof(elf32_ehdr_t)) { *err = EXEC_ENOEXEC; return 0; }

    uint8_t *filebuf = (uint8_t *)kmalloc(file->length);
    if (!filebuf) { *err = EXEC_ENOMEM; return 0; }

    ssize_t got = vfs_read(file, 0, file->length, filebuf);
    if (got != (ssize_t)file->length) {
        kfree(filebuf);
        *err = EXEC_ENOEXEC;
        return 0;
    }

    /* ---- Validate the ELF header and program headers ---------------- */
    elf32_ehdr_t *eh = (elf32_ehdr_t *)filebuf;
    int bad = 0;
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) bad = 1;
    else if (eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB) bad = 1;
    else if (eh->e_type != ET_EXEC || eh->e_machine != EM_386) bad = 1;
    else if (eh->e_phnum == 0 || eh->e_phoff == 0 ||
             eh->e_phentsize < sizeof(elf32_phdr_t)) bad = 1;
    else if ((uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > file->length) bad = 1;

    uint32_t max_pages = 0;
    for (uint16_t i = 0; !bad && i < eh->e_phnum; i++) {
        elf32_phdr_t *ph = (elf32_phdr_t *)(filebuf + eh->e_phoff + (uint32_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_filesz > ph->p_memsz ||
            (uint64_t)ph->p_offset + ph->p_filesz > file->length ||
            ph->p_vaddr < EXEC_USER_MIN ||
            (uint64_t)ph->p_vaddr + ph->p_memsz > EXEC_USER_MAX) {
            bad = 1;
            break;
        }
        uint32_t start = EXEC_PAGE_ALIGN_DOWN(ph->p_vaddr);
        uint32_t end   = EXEC_PAGE_ALIGN_UP(ph->p_vaddr + ph->p_memsz);
        max_pages += (end - start) / PAGE_SIZE;
    }
    if (!bad && (max_pages == 0 || eh->e_entry < EXEC_USER_MIN || eh->e_entry >= EXEC_USER_MAX)) {
        bad = 1;
    }
    if (bad) {
        kfree(filebuf);
        *err = EXEC_ENOEXEC;
        return 0;
    }

    exec_page_t *exec_pages = (exec_page_t *)kmalloc(max_pages * sizeof(exec_page_t));
    if (!exec_pages) {
        kfree(filebuf);
        *err = EXEC_ENOMEM;
        return 0;
    }
    uint32_t exec_page_count = 0;

    char name[PROCESS_NAME_LEN];
    basename_into(path, name, sizeof(name));

    /* Suspended: cannot run until process_start() below. */
    process_t *p = process_create_user_suspended(name, eh->e_entry, priority);
    if (!p) {
        kfree(exec_pages);
        kfree(filebuf);
        *err = EXEC_ENOMEM;
        return 0;
    }

    /* ---- Map and fill every PT_LOAD segment ------------------------- */
    int failed = 0;
    for (uint16_t i = 0; i < eh->e_phnum && !failed; i++) {
        elf32_phdr_t *ph = (elf32_phdr_t *)(filebuf + eh->e_phoff + (uint32_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint32_t flags = PTE_PRESENT | PTE_USER;
        if (ph->p_flags & PF_W) flags |= PTE_WRITABLE;

        uint32_t seg_start = EXEC_PAGE_ALIGN_DOWN(ph->p_vaddr);
        uint32_t seg_end   = EXEC_PAGE_ALIGN_UP(ph->p_vaddr + ph->p_memsz);

        for (uint32_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            if (vmm_get_physical(p->page_dir, (void *)va) != 0) {
                continue;   /* page shared with a previous segment */
            }
            void *phys = pmm_alloc_page();
            if (!phys) { failed = 1; break; }
            memset(phys, 0, PAGE_SIZE);   /* .bss and partial pages */
            vmm_map_page(p->page_dir, (void *)va, (uint32_t)phys, flags);
            exec_pages[exec_page_count].vaddr = va;
            exec_pages[exec_page_count].phys  = (uint32_t)phys;
            exec_pages[exec_page_count].flags = flags;
            exec_page_count++;
        }
        if (failed) break;

        /* Copy the file-backed bytes. The pages belong to p's page
         * directory, not the active one, so write them through their
         * physical (identity-mapped) addresses. */
        uint32_t remaining = ph->p_filesz;
        uint32_t file_off  = ph->p_offset;
        uint32_t dest_va   = ph->p_vaddr;
        while (remaining > 0) {
            uint32_t page_va  = EXEC_PAGE_ALIGN_DOWN(dest_va);
            uint32_t page_off = dest_va - page_va;
            uint32_t chunk    = PAGE_SIZE - page_off;
            if (chunk > remaining) chunk = remaining;

            uint32_t phys = vmm_get_physical(p->page_dir, (void *)page_va);
            memcpy((uint8_t *)phys + page_off, filebuf + file_off, chunk);

            dest_va   += chunk;
            file_off  += chunk;
            remaining -= chunk;
        }
    }

    uint32_t entry = eh->e_entry;
    kfree(filebuf);

    /* Hand the page list to the PCB first: on failure, process_kill() +
     * process_release() free the pages and the array with everything else. */
    p->exec_pages      = exec_pages;
    p->exec_page_count = exec_page_count;

    if (failed) {
        process_kill(p->pid);
        process_release(p);
        *err = EXEC_ENOMEM;
        return 0;
    }

    if (verbose) {
        console_write("exec: loaded '");
        console_write(path);
        console_write("' as pid ");
        print_num(p->pid, 10);
        console_write(", entry 0x");
        print_num(entry, 16);
        console_write(" (");
        print_num(exec_page_count, 10);
        console_write(" page(s))\n");
    }
    return p;   /* still suspended: caller calls process_start() */
}

/* ------------------------------------------------------------------ */
/* Embedded programs                                                    */
/* ------------------------------------------------------------------ */

/*
 * Each userland program is linked as its own ELF file and then turned
 * into an object with `objcopy -I binary` (see the Makefile). objcopy
 * names the start/end symbols after the file name.
 */
extern uint8_t _binary_hello_elf_start[];
extern uint8_t _binary_hello_elf_end[];
extern uint8_t _binary_FileCopy_elf_start[];
extern uint8_t _binary_FileCopy_elf_end[];

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} embedded_prog_t;

static void install_one(vfs_node_t *bin, const embedded_prog_t *prog) {
    uint32_t size = (uint32_t)(prog->end - prog->start);
    if (size == 0 || size > VFS_MAX_FILE_SIZE) {
        console_write("exec: embedded program has an invalid size\n");
        return;
    }
    vfs_node_t *f = vfs_find_node(bin, prog->name);
    if (!f) {
        f = vfs_create_in(bin, prog->name, NODE_FILE);
    }
    if (!f) {
        console_write("exec: could not create /bin entry\n");
        return;
    }
    vfs_truncate(f, 0);
    vfs_write(f, 0, size, prog->start);
    f->flags = VFS_FLAG_MODE_SET | 0755u;   /* rwxr-xr-x */
}

void exec_install_embedded_programs(void) {
    vfs_node_t *bin = vfs_find_node(vfs_get_root(), "bin");
    if (!bin) {
        console_write("exec: /bin missing, cannot install programs\n");
        return;
    }
    const embedded_prog_t progs[] = {
        { "hello",    _binary_hello_elf_start,    _binary_hello_elf_end    },
        { "FileCopy", _binary_FileCopy_elf_start, _binary_FileCopy_elf_end },
    };
    for (size_t i = 0; i < sizeof(progs) / sizeof(progs[0]); i++) {
        install_one(bin, &progs[i]);
    }
}
