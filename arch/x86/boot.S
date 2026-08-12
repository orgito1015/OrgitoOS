/*
 * OrgitoOS - Multiboot Kernel Loader
 * Boots via GRUB/QEMU in 32-bit protected mode and calls kmain()
 *
 * Higher-half bootstrap (see linker.ld's file header for the section
 * layout this depends on): _start below executes at its *physical* load
 * address (~1 MiB) with paging off - the rest of the kernel (everything
 * from higher_half_entry onward, including kmain and every C function in
 * the kernel) is linked to run from KERNEL_VIRTUAL_BASE (0xC0000000)
 * instead. Getting from one to the other means:
 *
 *   1. Build a temporary page directory using 4 MiB PSE pages that
 *      identity-maps the low 64 MiB of physical RAM at *both* virtual
 *      address 0 (so this code, still executing at its physical address,
 *      keeps working right up to and through the switch) and virtual
 *      address KERNEL_VIRTUAL_BASE (so the higher-half-linked code becomes
 *      reachable the instant paging turns on). 64 MiB matches memory/
 *      vmm.c's own identity-map cap exactly, so every physical page
 *      vmm_init() could need while building its *permanent* replacement
 *      directory - which happens later, already running higher-half -
 *      stays covered by this temporary one in the meantime.
 *   2. Enable PSE (CR4) and paging (CR0), still executing low.
 *   3. Jump to higher_half_entry, a label that lives in the normally-
 *      linked (and therefore higher-half) .text section - the one
 *      instruction in this whole file that actually moves EIP into the
 *      0xC0000000+ range.
 *
 * The temporary directory is never explicitly torn down - once memory/
 * vmm.c's vmm_init() builds and switches to the permanent one (which
 * includes an equivalent low-RAM identity map plus its own higher-half
 * alias for the kernel image specifically, memory/vmm.c PTE_USER carve-
 * out), nothing references the temporary one again; it just becomes a
 * few KiB of inert memory.
 */

/* Multiboot header constants */
.set ALIGN,    1<<0                    /* Align loaded modules on page boundaries */
.set MEMINFO,  1<<1                    /* Provide memory map */
.set VIDMODE,  1<<2                    /* Request a video mode (see fields below) */
.set FLAGS,    ALIGN | MEMINFO | VIDMODE /* Multiboot flags */
.set MAGIC,    0x1BADB002              /* Multiboot magic number */
.set CHECKSUM, -(MAGIC + FLAGS)        /* Checksum to verify multiboot header */

.set KERNEL_VIRTUAL_BASE, 0xC0000000
.set PDE_PRESENT,  1<<0
.set PDE_WRITABLE, 1<<1
.set PDE_PS,       1<<7                /* 4 MiB page (requires CR4.PSE)   */
.set CR4_PSE,      1<<4
.set CR0_PG,       1<<31
.set PSE_MAP_MIB,  64                  /* how much RAM to identity-map, matches memory/vmm.c */
.set PSE_CHUNKS,   PSE_MAP_MIB / 4     /* number of 4 MiB PDEs needed     */
.set PDE_HIGH_BASE, KERNEL_VIRTUAL_BASE >> 22  /* PDE index KERNEL_VIRTUAL_BASE starts at (768) */

/*
 * Multiboot header + this bootstrap code live in .text.boot, kept
 * unrelocated by linker.ld (VMA == LMA == physical) - see that file's
 * header comment. Must be within the first 8 KiB of the physical image.
 *
 * The video-mode fields below are only present/valid because FLAGS bit 2
 * is set - no address fields are needed since FLAGS bit 16 (AOUT_KLUDGE)
 * stays unset for this ELF-loaded kernel. mode_type=0 requests a linear
 * graphics framebuffer (not EGA text). This is only a *request*: if the
 * bootloader can't satisfy it, it may fall back to text mode instead -
 * drivers/framebuffer.c's fb_init() checks multiboot_info's actual
 * reported framebuffer_type/bpp before ever using it, and
 * drivers/console.c falls back to the legacy VGA text buffer whenever
 * that check fails, so an unsatisfied request degrades gracefully rather
 * than producing a blank/broken console.
 */
.section .text.boot
    .align 4
    .long MAGIC
    .long FLAGS
    .long CHECKSUM
    .long 0        /* mode_type: 0 = linear graphics framebuffer */
    .long 1024     /* width */
    .long 768      /* height */
    .long 32       /* depth (bits per pixel) */

.global _start
.type _start, @function

_start:
    /* Disable interrupts during boot */
    cli

    /*
     * Zero the temporary page directory, then fill PDE[0..15] (identity,
     * low) and PDE[768..783] (high alias, KERNEL_VIRTUAL_BASE) with 4 MiB
     * PSE entries for the same 16 physical 4 MiB chunks (0 .. 64 MiB).
     * boot_pgdir_phys is itself unrelocated (see .bss.boot below), so
     * this label already evaluates to a physical address - no adjustment
     * needed, unlike symbols in the higher-half-linked sections.
     */
    movl $boot_pgdir_phys, %edi
    xor %eax, %eax
    mov $1024, %ecx
    rep stosl

    xor %edx, %edx                     /* chunk index, 0 .. PSE_CHUNKS-1 */
.Lfill_pgdir:
    mov %edx, %eax
    shl $22, %eax                      /* eax = chunk index * 4 MiB      */
    or $(PDE_PRESENT | PDE_WRITABLE | PDE_PS), %eax

    movl $boot_pgdir_phys, %edi
    mov %eax, (%edi, %edx, 4)                          /* low alias  */
    mov %eax, (PDE_HIGH_BASE * 4)(%edi, %edx, 4)        /* high alias */

    inc %edx
    cmp $PSE_CHUNKS, %edx
    jl .Lfill_pgdir

    /* Load CR3, enable PSE, enable paging - still executing at our
     * physical address the whole time (covered by the low alias). */
    movl $boot_pgdir_phys, %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or $CR4_PSE, %eax
    mov %eax, %cr4

    mov %cr0, %eax
    or $CR0_PG, %eax
    mov %eax, %cr0

    /*
     * Paging is now active. Jump into the higher-half-linked
     * continuation - a plain near jump works correctly here even though
     * the target is ~3 GiB away: x86 computes the rel32 displacement as
     * ordinary 32-bit wraparound arithmetic, so it's exact for any
     * target regardless of "distance". No segment reload needed either;
     * CS is untouched throughout (paging is orthogonal to segmentation).
     */
    jmp higher_half_entry

/*
 * Temporary boot-time page directory - see the block comment above.
 * Lives in .bss.boot (unrelocated, like .text.boot) so its label is a
 * physical address usable directly by the pre-paging code above.
 */
.section .bss.boot
    .align 4096
    .global boot_pgdir_phys
boot_pgdir_phys:
    .space 4096

/*
 * Normal, higher-half-linked kernel code from here on - this is the
 * *only* label in this file living in the ordinarily-relocated .text
 * section, reached via the jmp above once paging (and therefore the
 * higher-half virtual address range) is active.
 */
.section .text
    .extern kmain

.global higher_half_entry
.type higher_half_entry, @function
higher_half_entry:
    /* Set up kernel stack (grows downward from stack_top - an ordinary
     * higher-half .bss symbol now, valid through the high alias). */
    mov $stack_top, %esp

    /* Push Multiboot info pointer (passed in ebx by GRUB) - a physical
     * address, dereferenced later (memory/pmm.c etc.) through the low
     * alias, which stays active continuously through this whole
     * temporary-directory window and its eventual permanent replacement
     * (memory/vmm.c vmm_init()). */
    push %ebx

    /* Call C kernel entry point */
    call kmain

    /* Kernel should never return, but if it does, halt */
.Lhalt:
    cli
    hlt
    jmp .Lhalt

/* Kernel stack section (uninitialized data) - ordinarily relocated
 * (higher-half), unlike boot_pgdir_phys above. */
.section .bss
    .align 16
stack_bottom:
    .space 16384       /* 16 KiB stack */
stack_top:
