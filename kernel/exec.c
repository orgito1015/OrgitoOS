/*
 * OrgitoOS - exec(): load a real ELF32 binary into a process's own
 * address space.
 *
 * Every ring 3 "process" up to now (kernel/user_programs.c) has been a
 * plain C function linked straight into the kernel image and reached
 * through the small user-executable carve-out of the identity map
 * ([kernel_start, rodata_start) in memory/vmm.c). This file is the first
 * code path that instead reads a file from the VFS, parses it as ELF32,
 * and maps its PT_LOAD segments into a private set of freshly allocated
 * physical pages - a real loaded binary, not a function pointer into the
 * kernel.
 *
 * Design note - spawn, not in-place replace: POSIX exec() replaces the
 * calling process's own image (same pid). This implementation instead
 * creates a brand-new process (like posix_spawn()), because it composes
 * cleanly with the rest of process.c/vmm.c as they exist today without
 * touching context_switch's syscall-return path.
 *
 * A process's ELF segments are private, unshared mappings in its own
 * page_dir (see process_t.exec_pages). sys_fork() (kernel/syscall.c)
 * deep-copies them into the child's own page_dir at the identical virtual
 * addresses - the same trick already used for the fixed user-stack window
 * (PROCESS_USTACK_VIRT_BASE) - so "fork(), then have the child exec()",
 * the common Unix process-spawn idiom, works: each side ends up with its
 * own private physical pages, and mutating one never touches the other's.
 *
 * The syscall is still named SYS_EXEC (kernel/syscall.h) to match the
 * roadmap wording ("exec() ... not loaded binaries"); what changed is
 * real ELF loading, not process identity semantics.
 */

#include "exec.h"
#include "string.h"
#include "../drivers/console.h"
#include "../fs/vfs.h"
#include "../memory/heap.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"

/* ------------------------------------------------------------------ */
/* Minimal ELF32 structures (no libc/system elf.h in a freestanding    */
/* build, so declare just enough to load a static executable)          */
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

#define EXEC_PAGE_ALIGN_DOWN(x) ((uint32_t)(x) & ~(PAGE_SIZE - 1u))
#define EXEC_PAGE_ALIGN_UP(x)   (((uint32_t)(x) + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u))

/* Basename of a VFS path, for the process table's name[] field. */
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

process_t *process_exec(const char *path, uint8_t priority) {
    if (!path || path[0] != '/') {
        console_write("exec: path must be absolute\n");
        return 0;
    }

    vfs_node_t *file = vfs_resolve_path(path);
    if (!file) {
        console_write("exec: ");
        console_write(path);
        console_write(": No such file or directory\n");
        return 0;
    }
    if (file->type != NODE_FILE) {
        console_write("exec: ");
        console_write(path);
        console_write(": Is a directory\n");
        return 0;
    }
    if (file->length < sizeof(elf32_ehdr_t)) {
        console_write("exec: file too small to be an ELF binary\n");
        return 0;
    }

    uint8_t *filebuf = (uint8_t *)kmalloc(file->length);
    if (!filebuf) {
        console_write("exec: out of memory reading file\n");
        return 0;
    }

    ssize_t got = vfs_read(file, 0, file->length, filebuf);
    if (got < (ssize_t)sizeof(elf32_ehdr_t)) {
        console_write("exec: failed to read file\n");
        kfree(filebuf);
        return 0;
    }

    elf32_ehdr_t *eh = (elf32_ehdr_t *)filebuf;
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        console_write("exec: not an ELF file\n");
        kfree(filebuf);
        return 0;
    }
    if (eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB) {
        console_write("exec: not a 32-bit little-endian ELF\n");
        kfree(filebuf);
        return 0;
    }
    if (eh->e_type != ET_EXEC) {
        console_write("exec: not a static executable (ET_EXEC) - PIE/shared objects are not supported\n");
        kfree(filebuf);
        return 0;
    }
    if (eh->e_machine != EM_386) {
        console_write("exec: not an x86 (EM_386) binary\n");
        kfree(filebuf);
        return 0;
    }
    if (eh->e_phnum == 0 || eh->e_phoff == 0 ||
        eh->e_phentsize < sizeof(elf32_phdr_t)) {
        console_write("exec: no program headers\n");
        kfree(filebuf);
        return 0;
    }
    if ((uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > file->length) {
        console_write("exec: program headers run past end of file\n");
        kfree(filebuf);
        return 0;
    }

    /* Upper bound on the number of distinct pages every PT_LOAD segment
     * could touch, for sizing the tracking array up front. Some entries
     * may end up unused if segments share a page - exec_page_count (not
     * the array length) is authoritative for how many are actually live. */
    uint32_t max_pages = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        elf32_phdr_t *ph = (elf32_phdr_t *)(filebuf + eh->e_phoff + (uint32_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        uint32_t start = EXEC_PAGE_ALIGN_DOWN(ph->p_vaddr);
        uint32_t end   = EXEC_PAGE_ALIGN_UP((uint64_t)ph->p_vaddr + ph->p_memsz);
        max_pages += (end - start) / PAGE_SIZE;
    }
    if (max_pages == 0) {
        console_write("exec: no loadable segments\n");
        kfree(filebuf);
        return 0;
    }

    exec_page_t *exec_pages = (exec_page_t *)kmalloc(max_pages * sizeof(exec_page_t));
    if (!exec_pages) {
        console_write("exec: out of memory\n");
        kfree(filebuf);
        return 0;
    }
    uint32_t exec_page_count = 0;

    char name[PROCESS_NAME_LEN];
    basename_into(path, name, sizeof(name));

    process_t *p = process_create_user(name, (void (*)(void))(uintptr_t)eh->e_entry, priority);
    if (!p) {
        console_write("exec: could not create process\n");
        kfree(exec_pages);
        kfree(filebuf);
        return 0;
    }

    int failed = 0;
    for (uint16_t i = 0; i < eh->e_phnum && !failed; i++) {
        elf32_phdr_t *ph = (elf32_phdr_t *)(filebuf + eh->e_phoff + (uint32_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint32_t flags = PTE_PRESENT | PTE_USER;
        if (ph->p_flags & PF_W) flags |= PTE_WRITABLE;

        uint32_t seg_start = EXEC_PAGE_ALIGN_DOWN(ph->p_vaddr);
        uint32_t seg_end   = EXEC_PAGE_ALIGN_UP((uint64_t)ph->p_vaddr + ph->p_memsz);

        for (uint32_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            if (vmm_get_physical(p->page_dir, (void *)va) != 0) {
                /* Already backed (segments sharing a page-aligned page) */
                continue;
            }
            void *phys = pmm_alloc_page();
            if (!phys) {
                console_write("exec: out of physical memory\n");
                failed = 1;
                break;
            }
            memset(phys, 0, PAGE_SIZE);   /* covers .bss and partial pages */
            vmm_map_page(p->page_dir, (void *)va, (uint32_t)phys, flags);
            exec_pages[exec_page_count].vaddr = va;
            exec_pages[exec_page_count].phys  = (uint32_t)phys;
            exec_pages[exec_page_count].flags = flags;
            exec_page_count++;
        }
        if (failed) break;

        /* Copy this segment's file-backed bytes (p_filesz, may be < p_memsz
         * for .bss) into the pages just mapped. The pages live in `p`'s own
         * page_dir, not the currently-active one (this loader runs as
         * whichever kernel-mode context called process_exec - the shell
         * thread, or a syscall on the caller's own kernel stack), so reach
         * them through vmm_get_physical() + the kernel's identity map
         * rather than a direct pointer into p_vaddr. */
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

    kfree(filebuf);

    if (failed) {
        for (uint32_t i = 0; i < exec_page_count; i++) {
            pmm_free_page((void *)exec_pages[i].phys);
        }
        kfree(exec_pages);
        /* Never scheduled yet in any meaningful way beyond being enqueued;
         * dequeue it and free everything else right away instead of
         * leaving an unreapable zombie behind (no parent will wait() for
         * a spawn that never got to run). */
        process_kill(p->pid);
        process_release(p);
        return 0;
    }

    p->exec_pages      = exec_pages;
    p->exec_page_count = exec_page_count;

    console_write("exec: loaded '");
    console_write(path);
    console_write("' as pid ");
    {
        char buf[12];
        itoa((int)p->pid, buf, 10);
        console_write(buf);
    }
    console_write(", entry 0x");
    {
        char buf[12];
        itoa((int)eh->e_entry, buf, 16);
        console_write(buf);
    }
    console_write(" (");
    {
        char buf[12];
        itoa((int)exec_page_count, buf, 10);
        console_write(buf);
    }
    console_write(" page(s))\n");

    return p;
}

/* ------------------------------------------------------------------ */
/* Embedded demo binary                                                 */
/* ------------------------------------------------------------------ */

/*
 * Built from userland/hello.c by the Makefile's userland rules: compiled
 * freestanding and linked as a real ELF32 executable (userland/user.ld),
 * then objcopy'd into the kernel image as a plain byte blob. These
 * symbols mark its start/end in the running kernel's own memory.
 */
extern uint8_t _binary_hello_elf_start[];
extern uint8_t _binary_hello_elf_end[];

void exec_install_embedded_programs(void) {
    vfs_node_t *root = vfs_get_root();
    vfs_node_t *bin = vfs_find_node(root, "bin");
    if (!bin) {
        console_write("exec: /bin missing, cannot preload demo binary\n");
        return;
    }

    uint32_t size = (uint32_t)(_binary_hello_elf_end - _binary_hello_elf_start);
    if (size == 0 || size > VFS_MAX_FILE_SIZE) {
        console_write("exec: embedded hello binary has an invalid size\n");
        return;
    }

    vfs_node_t *hello = vfs_create_node("hello", NODE_FILE);
    if (!hello) {
        console_write("exec: could not create /bin/hello node\n");
        return;
    }
    vfs_write(hello, 0, size, _binary_hello_elf_start);
    vfs_add_child(bin, hello);

    console_write("exec: preloaded /bin/hello (");
    {
        char buf[12];
        itoa((int)size, buf, 10);
        console_write(buf);
    }
    console_write(" bytes) - a real ELF32 binary, run it with: exec /bin/hello\n");
}
