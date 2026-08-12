/*
 * OrgitoOS Linker Script
 *
 * Higher-half kernel, dual-mapped (see memory/vmm.c's vmm_init() for the
 * "dual-mapped, not pure higher-half" scope decision): the bulk of the
 * kernel is linked to run from KERNEL_VIRTUAL_BASE (0xC0000000) while its
 * physical load address stays at 1 MiB - VMA (where the linker resolves
 * every symbol/instruction address to) and LMA (where the bytes actually
 * live in the loaded file/physical RAM) diverge by exactly
 * KERNEL_VIRTUAL_BASE for every section from .text onward, via the AT()
 * directives below. This is the standard "Higher Half Bare Bones"
 * pattern (see the OSDev wiki article of the same name, already linked
 * from README.md's further-reading table).
 *
 * .text.boot/.bss.boot are the one exception: a small amount of code and
 * one scratch page directory (arch/x86/boot.S) that must run and be
 * addressable *before* paging is enabled, i.e. while the CPU can only see
 * physical addresses - VMA == LMA == physical for these two, no
 * KERNEL_VIRTUAL_BASE offset at all. Everything in them is responsible
 * for getting paging turned on (via a temporary identity+high-alias
 * mapping) and then jumping into the normally-linked, higher-half code
 * below - see boot.S's own comments for the mechanics.
 */

ENTRY(_start)

KERNEL_VIRTUAL_BASE = 0xC0000000;

SECTIONS
{
  /* Load the kernel at 1 MiB (0x100000) - the physical address GRUB/QEMU
   * place it at and where _start begins executing with paging off. */
  . = 1M;

  kernel_start_phys = .;

  /*
   * Multiboot header (must be in the first 8 KiB of the physical image)
   * plus boot.S's pre-paging bootstrap code and its scratch page
   * directory. Unrelocated: VMA == LMA == physical here.
   */
  .text.boot :
  {
    *(.multiboot)
    *(.text.boot)
  }
  .bss.boot ALIGN(4K) :
  {
    *(.bss.boot)
  }

  /*
   * From here on, every section's VMA jumps to KERNEL_VIRTUAL_BASE while
   * its LMA (via AT()) continues the physical placement seamlessly right
   * after .bss.boot - i.e. the bytes stay contiguous in the loaded file/
   * physical RAM, but every symbol in them resolves to a higher-half
   * virtual address. boot.S's PSE pivot jumps here once paging is active.
   */
  . += KERNEL_VIRTUAL_BASE;

  /* Virtual start of the higher-half kernel image. memory/pmm.c/vmm.c use
   * the *_phys symbols below (not this one) for physical-address work;
   * this one is for anything that needs the kernel's own virtual range,
   * e.g. memory/vmm.c's high-alias mapping call. */
  kernel_start = .;

  .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRTUAL_BASE)
  {
    *(.text*)
  }

  /* Read-only data section - contains string literals and const data
   * Aligned to 4 KiB for memory protection.
   *
   * rodata_start marks the page-aligned boundary where the kernel's
   * user-executable .text ends. memory/vmm.c maps [kernel_start,
   * rodata_start) at its higher-half address as the only range that
   * keeps PTE_USER, so ring 3 can fetch its own (kernel-linked) code but
   * nothing from here onward - kernel heap, PCBs, kernel stacks, and now
   * .rodata/.data/.bss - is reachable from ring 3.
   */
  .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRTUAL_BASE)
  {
    rodata_start = .;
    *(.rodata*)
  }

  /* Initialized data section - contains global/static variables
   * with initial values
   */
  .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE)
  {
    *(.data*)
  }

  /* Uninitialized data section - contains global/static variables
   * without initial values (zeroed by bootloader)
   * COMMON is for uninitialized C data
   */
  .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VIRTUAL_BASE)
  {
    *(.bss*)
    *(COMMON)
  }

  /* Virtual end of the kernel image (exclusive). */
  kernel_end = .;

  /*
   * Physical-address counterparts of rodata_start/kernel_end
   * (kernel_start_phys is already physical, defined above before the
   * KERNEL_VIRTUAL_BASE jump). memory/pmm.c uses [kernel_start_phys,
   * kernel_end_phys) to reserve the kernel's own page frames; memory/
   * vmm.c uses all three *_phys symbols as the physical backing for its
   * higher-half alias mapping. Plain linker-computed arithmetic - no C
   * code anywhere needs to subtract KERNEL_VIRTUAL_BASE itself.
   */
  rodata_start_phys = rodata_start - KERNEL_VIRTUAL_BASE;
  kernel_end_phys    = kernel_end - KERNEL_VIRTUAL_BASE;

  /* Discard unnecessary sections */
  /DISCARD/ :
  {
    *(.note.gnu.build-id)
    *(.comment)
    *(.eh_frame)
  }
}
