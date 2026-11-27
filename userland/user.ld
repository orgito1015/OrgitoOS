/*
 * OrgitoOS - Linker script for standalone userland ELF binaries.
 *
 * Produces a statically linked ET_EXEC ELF32 executable that
 * kernel/exec.c loads into a new process. The output is never linked
 * into the kernel itself; it is embedded as a byte blob and installed in
 * /bin at boot (see the Makefile's userland rules).
 *
 * Load address 0x40000000 (1 GiB): clear of the kernel's identity map
 * (capped at 64 MiB, memory/vmm.c) and of the per-process user stack
 * window (0x08000000, process/process.h).
 *
 * Two PT_LOAD segments:
 *   text  (R-X): .text, .rodata         -> mapped read-only
 *   data  (RW-): .data, .bss            -> mapped writable, .bss zero-filled
 * so a program cannot overwrite its own code (exec.c only sets
 * PTE_WRITABLE for segments with PF_W).
 */

ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x40000000;

    .text   : { *(.text*) }   :text
    .rodata : { *(.rodata*) } :text

    . = ALIGN(4096);
    .data   : { *(.data*) }   :data
    .bss    : { *(.bss*) *(COMMON) } :data

    /DISCARD/ :
    {
        *(.note.gnu.build-id)
        *(.note.GNU-stack)
        *(.comment)
        *(.eh_frame)
    }
}
