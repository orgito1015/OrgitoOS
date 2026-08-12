/*
 * OrgitoOS - Linker script for standalone userland ELF binaries.
 *
 * Produces a real, statically-linked ET_EXEC ELF32 executable loaded by
 * kernel/exec.c - unlike linker.ld (the kernel image itself), the output
 * here is never linked into orgitoos.bin directly; it is objcopy'd into
 * a byte blob and embedded separately (see the Makefile's userland
 * rules).
 *
 * Load address: 0x40000000 (1 GiB). Chosen well clear of both the
 * kernel's identity map (capped at 64 MiB, memory/vmm.c vmm_init) and
 * the fixed per-process user stack window (PROCESS_USTACK_VIRT_BASE =
 * 0x08000000, process/process.h), so a loaded binary's code/data can
 * never collide with either.
 *
 * A single PHDRS entry deliberately assigns every section to the same
 * PT_LOAD segment: without an explicit PHDRS block, GNU ld's default
 * behavior page-aligns the VMA of each new segment, which also forces
 * the FILE offset to jump to the next page boundary - harmless for a
 * normal executable on disk, but the VFS ramfs backing /bin/hello caps
 * file size at 4 KiB (VFS_MAX_FILE_SIZE), and multiple page-aligned
 * segment gaps alone would blow past that from padding, not code. One
 * segment keeps the file exactly as large as the actual code and data.
 */

ENTRY(_start)

PHDRS
{
    load PT_LOAD FLAGS(7); /* PF_X | PF_W | PF_R */
}

SECTIONS
{
    . = 0x40000000;

    .text : { *(.text*) } :load
    .rodata : { *(.rodata*) } :load
    .data : { *(.data*) } :load
    .bss : { *(.bss*) *(COMMON) } :load

    /DISCARD/ :
    {
        *(.note.gnu.build-id)
        *(.comment)
        *(.eh_frame)
    }
}
