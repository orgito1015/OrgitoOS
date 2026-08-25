/*
 * OrgitoOS Linker Script
 * Defines the memory layout and section arrangement for the kernel
 */

/* Entry point symbol from boot.S */
ENTRY(_start)

/* Explicit program headers so .text/.rodata/.data+.bss land in separate
 * PT_LOAD segments with distinct permissions (R+X / R / R+W) instead of
 * the linker's default of collapsing same-region output sections into one
 * segment - which, combined with --nmagic, produced a single RWX segment
 * (harmless here since GRUB doesn't enforce ELF segment permissions and
 * memory/vmm.c enforces the kernel's real W^X split later in software, but
 * worth doing properly). FLAGS values are the standard ELF PT_LOAD bits:
 * PF_X=1, PF_W=2, PF_R=4. */
PHDRS
{
  text_phdr   PT_LOAD FLAGS(5); /* R+X */
  rodata_phdr PT_LOAD FLAGS(4); /* R   */
  data_phdr   PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS
{
  /* Load the kernel at 1 MiB (0x100000)
   * This is a standard location above conventional memory
   * and below the 4 GiB boundary for 32-bit addressing
   */
  . = 1M;

  /* Physical start of the kernel image. The PMM uses this (together with
   * kernel_end below) to reserve the kernel's own page frames so the
   * allocator never hands out memory the running kernel occupies. */
  kernel_start = .;

  /* Multiboot header must be in the first 8 KiB of the kernel
   * This section contains the Multiboot magic, flags, and checksum
   * It MUST be placed at the very start before all other sections
   */
  .text :
  {
    /* Place multiboot header first, before any code */
    *(.multiboot)
    /* Then place all code sections */
    *(.text*)
  } :text_phdr

  /* Read-only data section - contains string literals and const data
   * Aligned to 4 KiB for memory protection.
   *
   * rodata_start marks the page-aligned boundary where the kernel's
   * user-executable .text ends. memory/vmm.c uses [kernel_start,
   * rodata_start) as the only identity-mapped range that keeps PTE_USER,
   * so ring 3 can fetch its own (kernel-linked) code but nothing from
   * here onward - kernel heap, PCBs, kernel stacks, and now .rodata/.data/
   * .bss - is reachable from ring 3.
   */
  .rodata ALIGN(4K) :
  {
    rodata_start = .;
    *(.rodata*)
  } :rodata_phdr

  /* Initialized data section - contains global/static variables
   * with initial values
   */
  .data ALIGN(4K) :
  {
    *(.data*)
  } :data_phdr

  /* Uninitialized data section - contains global/static variables
   * without initial values (zeroed by bootloader)
   * COMMON is for uninitialized C data
   */
  .bss ALIGN(4K) :
  {
    *(.bss*)
    *(COMMON)
  } :data_phdr

  /* Physical end of the kernel image (exclusive). Everything in
   * [kernel_start, kernel_end) is the loaded kernel and must be
   * reserved by the physical memory manager. */
  kernel_end = .;

  /* Discard unnecessary sections */
  /DISCARD/ :
  {
    *(.note.gnu.build-id)
    *(.comment)
    *(.eh_frame)
  }
}
