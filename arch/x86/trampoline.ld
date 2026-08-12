/*
 * OrgitoOS - Linker script for the AP real-mode trampoline
 * (arch/x86/trampoline.S).
 *
 * Fixed load address 0x8000: this is a physical address, not a virtual
 * one - the trampoline runs before paging is enabled on the booting AP.
 * It must be below 1 MiB (STARTUP IPI real-mode addressing), 4 KiB
 * aligned (the SIPI vector byte encodes address >> 12), and inside the
 * kernel's identity-mapped low-memory range so the BSP (which *is*
 * paged) can still write the trampoline bytes and the AP's boot
 * parameters there through an ordinary pointer.
 */

ENTRY(ap_trampoline_start)

SECTIONS
{
    . = 0x8000;

    .text : {
        *(.text*)
    }

    /DISCARD/ :
    {
        *(.note.gnu.build-id)
        *(.comment)
        *(.eh_frame)
    }
}
