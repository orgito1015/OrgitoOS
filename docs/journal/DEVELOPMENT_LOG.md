# Development Log

This journal tracks the day-to-day progress, learnings, and challenges encountered during OrgitoOS development. Each entry documents what was learned, what was implemented, issues encountered, and invariants strengthened.

---

## 2024-XX-XX: Initial Documentation Refactoring

### Learned
- Documentation structure is critical for long-term maintainability
- Separating learning notes from architectural documentation improves clarity
- Chapter-based learning organization helps track knowledge progression
- Journal format is useful for tracking incremental progress

### Implemented
- Created new documentation directory structure
- Moved existing documentation to appropriate subdirectories
- Created chapter templates for systematic learning
- Established learning index for tracking progress
- Set up development journal for ongoing documentation

### Issues
- None during refactoring
- Need to ensure cross-references remain valid after file moves

### Invariants Strengthened
- Documentation must be organized and discoverable
- Learning progression should be explicit and tracked
- Architectural decisions must be documented
- Development history should be preserved

---

## Phase 0 Implementation (Historical Summary)

### Learned
- x86 boot process and protected mode initialization
- Multiboot specification requirements and alignment constraints
- Interrupt descriptor table structure and interrupt handling
- Physical and virtual memory management techniques
- Hardware driver development patterns (console, keyboard, timer)

### Implemented
- Multiboot-compliant kernel entry point
- Complete exception handling for all 32 x86 exceptions
- Interrupt handling infrastructure (IDT, ISR, PIC)
- Physical memory manager with bitmap allocator
- Virtual memory manager with two-level paging
- VGA text mode console driver
- PS/2 keyboard driver with line buffering
- PIT timer driver at 100 Hz
- Kernel panic handler with detailed crash reports

### Issues
- Multiboot header alignment required careful linker script configuration
- Page table management complexity during early boot
- Interrupt handler assembly required precise stack frame management
- Keyboard interrupt timing sensitive to PIC configuration
- Memory detection via multiboot required validation

### Invariants Strengthened
- Kernel must validate multiboot magic number before proceeding
- All exceptions must be handled, even if just reporting and halting
- Memory allocations must check for null returns
- Interrupts must acknowledge EOI to PIC
- Page tables must be properly aligned
- All hardware initialization must be in correct sequence

---

## Boot Fix & New Features (Historical Summary)

Summary of a test/fix/feature pass over the kernel, built with `make` and
boot-tested in QEMU (`qemu-system-i386`) through to the interactive
`OrgitoOS>` shell.

### Critical bug fixed — kernel did not boot (triple fault -> reboot loop)

**Symptom:** on boot the CPU took a #GP (v=0d), then a double fault (v=08),
then a triple fault, resetting in a loop. Screen stayed blank.

**Root cause:** the physical memory manager never reserved the kernel's own
page frames. The kernel loads at physical `0x100000` and its image spans
~3.95 MiB (up to ~`0x4C5BC8`, including the 1 MiB PMM bitmap in BSS).
`pmm_init` marked everything below 1 MiB used and everything from `0x100000`
up as *free* — i.e. the memory the running kernel occupies. The first
`pmm_alloc_page()` in `vmm_init()` therefore returned `0x100000` and the page
directory was written straight over live kernel code/data, corrupting it and
faulting.

**Fix:**
- `linker.ld`: export `kernel_start` / `kernel_end` symbols around the image.
- `memory/pmm.c`: added `reserve_range()` and reserve both page 0 (null / IVT
  / BDA) and `[kernel_start, kernel_end)` in `pmm_init`, in both the mmap and
  the no-mmap fallback paths. Used-page count is recomputed after
  reservation.

### Learned
- A PMM that doesn't reserve the kernel's own image will happily hand its
  first free page right back to the VMM's initial page-directory allocation
  — silent self-corruption, not an obviously "wrong" API call.
- `[1/N]`-style boot step counters drift out of sync with actual step count
  as subsystems are added; worth double-checking after every new init call.

### Implemented
- Kernel image self-reservation in the PMM (see above).
- Serial console (COM1): `drivers/serial.c`/`drivers/serial.h`, 16550 UART,
  38400 8N1, with a loopback self-test. The console driver mirrors all
  output to COM1, so the kernel is usable headless and `make debug`/
  `make qemu-log` capture a full boot/session log
  (`qemu-system-i386 -cdrom orgitoos.iso -serial stdio`).
- New shell commands exposing filesystem operations the VFS already
  supported internally, plus memory reporting:

  | Command | Description |
  |---------|-------------|
  | `mkdir <name>...` | Create directories |
  | `touch <name>...` | Create empty files |
  | `write <file> <text...>` | Write text to a file (creates it if needed) |
  | `rm <name>...` | Remove files / directory entries |
  | `meminfo` | Report physical (PMM) and kernel heap usage |

  Supporting change: `memory/heap.c`/`memory/heap.h` gained
  `heap_get_stats()` (walks the free list to report total/used/free bytes
  and block count) so `meminfo` can show heap state.

### Verified session (over serial)
```
OrgitoOS> meminfo
Physical memory (PMM, 4 KiB pages)
  total : 32736 pages (130944 KiB)
  used  : 2265 pages (9060 KiB)
  free  : 30471 pages (121884 KiB)
Kernel heap (first-fit free list)
  total : 4194288 bytes
  used  : 0 bytes
  free  : 4194288 bytes across 1 block(s)
OrgitoOS> mkdir projects
OrgitoOS> cd projects
OrgitoOS> write notes.txt hello orgitoos world
OrgitoOS> ls -l
-        19  notes.txt
OrgitoOS> cat notes.txt
hello orgitoos world
```

### Invariants Strengthened
- The PMM must reserve the kernel's own loaded image (and page 0) before
  handing out any free frames — no allocator call may ever return memory the
  running kernel occupies.
- Boot step counters must match the actual number of init steps.

---

## Future Entry Template

## YYYY-MM-DD: [Topic/Feature]

### Learned
- [Key concepts understood today]
- [Insights gained from research or implementation]
- [Understanding of system behavior]

### Implemented
- [Code changes made]
- [Features added]
- [Components completed]

### Issues
- [Problems encountered]
- [Bugs found and fixed]
- [Unexpected behavior]
- [Workarounds needed]

### Invariants Strengthened
- [New invariants established]
- [Existing invariants validated]
- [Safety properties proven]
- [Error handling improved]

---

## Development Guidelines

### Journal Entry Best Practices
- Make entries after significant work sessions
- Be specific about what was learned
- Document both successes and failures
- Note surprising behaviors or unexpected results
- Reference relevant code locations
- Link to related documentation
- Update regularly, don't batch weeks of work

### What to Document

#### Always Include
- New features or components
- Bug fixes and their root causes
- Performance optimizations
- Design decisions and rationale
- Failed approaches and why they didn't work

#### Optionally Include
- Interesting debugging sessions
- Research findings
- Third-party code integration
- Tool or environment changes
- Refactoring efforts

### Invariants Section

Invariants are properties that must always hold true in the system. Examples:
- Memory allocations are always page-aligned
- Interrupts are disabled during critical sections
- Page tables are always mapped in kernel space
- Process IDs are unique and non-zero
- Kernel stack never overflows
- All locks are released before returning to user space

Strengthening invariants means:
- Adding checks to verify invariants
- Improving error handling
- Adding assertions
- Documenting assumptions
- Making implicit contracts explicit

### Example Entry (Reference)

## 2024-01-15: Virtual Memory Manager Implementation

### Learned
- Two-level paging requires careful management of page directory and tables
- TLB must be flushed after page table modifications
- Identity mapping simplifies early boot process
- Page fault handler must distinguish between valid and invalid accesses

### Implemented
- vmm_init(): Initialize paging with identity mapping
- vmm_map_page(): Map virtual to physical addresses
- vmm_unmap_page(): Unmap virtual addresses
- vmm_alloc_page(): Allocate and map new pages
- Page fault exception handler with detailed reporting
- TLB invalidation functions (invlpg, full flush)

### Issues
- Initial implementation forgot to set present bit in PTEs
- Page directory allocation during early boot required careful ordering
- Recursive page table mapping was complex, used simpler approach
- TLB coherency issues when not flushing after unmap

### Invariants Strengthened
- All page directory entries must be checked for present bit before use
- Page tables must be zeroed after allocation
- TLB must be flushed after any page table modification
- Virtual addresses passed to VMM must be page-aligned
- Physical addresses from PMM are guaranteed to be page-aligned

---

## Commit Message Guidelines

When committing code, follow these patterns:

### Format
```
[Component] Brief description

Detailed explanation of changes, reasoning, and any
important implementation notes.

Relates to: [Issue #] or [Feature Name]
```

### Examples
```
[Memory] Implement virtual memory manager

Add complete VMM with two-level paging, including:
- Page directory and page table management
- Page mapping and unmapping functions
- TLB invalidation
- Page fault handler

Relates to: Phase 0 Memory Management
```

```
[Interrupts] Fix timer interrupt frequency

Timer was running at 1000 Hz causing excessive interrupts.
Reduced to 100 Hz for better performance while maintaining
adequate timing resolution for future scheduler.

Relates to: Timer Driver Optimization
```

---

*Journal started: [Date of initial entry]*
*Last updated: [Date of last entry]*
