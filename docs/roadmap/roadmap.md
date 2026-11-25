# OrgitoOS Development Roadmap

## Current Status

✅ **Completed Features:**
- Multiboot-compliant bootloader, 32-bit protected mode kernel
- VGA text mode terminal with scrolling, PS/2 keyboard driver, serial console (COM1)
- Full interrupt system: IDT, PIC remapping, all 32 x86 exception handlers
- PIT timer (100 Hz) with tick counting and sleep
- Physical memory manager (bitmap allocator) and virtual memory manager (two-level paging)
- Kernel heap allocator (`kmalloc`/`kfree`, first-fit free list)
- Process management: PCBs, multilevel round-robin scheduler with aging, ring 3 user
  mode via GDT/TSS, `int 0x80` syscalls (`exit`/`write`/`getpid`/`fork`/`yield`/`sleep`/
  `getppid`/`wait`) — ring-3 programs run correctly since paging is actually enabled
  (see Phase 5 below)
- In-memory VFS (ramfs) with directories, files, and coreutils-style shell commands
  (`ls`, `cat`, `stat`, `mkdir`, `touch`, `write`, `rm`)
- IPC (pipes, message queues), CPU-count detection (SMP), a simulated in-memory GUI
  framebuffer, networking data structures, and shell scripting with variables

This roadmap previously undercounted progress — several phases below were marked
"planned" long after being implemented. Status is corrected to match the code.

## Phase 1: Core Interrupt System — ✅ Complete
- [x] IDT initialization, PIC remapping
- [x] Keyboard interrupt handler (IRQ1), scan code translation, Shift/Caps Lock, line buffering
- [x] Exception handlers for all 32 x86 exceptions (`arch/x86/exceptions.c`)
- [x] Double fault handler (exception vector 8)

## Phase 2: Timer and Basic Scheduling — ✅ Complete
- [x] PIT initialization at 100 Hz (`drivers/timer.c`)
- [x] Timer interrupt handler (IRQ0), global tick counter, `process_sleep()`
- [x] Scheduler tick integrated with the timer (`scheduler_tick()`)

## Phase 3: Memory Management — ✅ Complete
- [x] Multiboot memory map parsing, bitmap-based physical page allocator (`memory/pmm.c`)
- [x] Two-level paging, identity mapping, page fault handler (`memory/vmm.c`)
- [x] Kernel heap: first-fit free-list allocator with stats (`memory/heap.c`)
- [ ] Higher-half kernel mapping — `KERNEL_VIRTUAL_BASE` is defined but unused; the
  kernel currently runs identity-mapped, not higher-half

## Phase 4: Process Management — ✅ Complete
- [x] `process_t` PCB with register state, PID/PPID allocation, process states
  (ready/running/blocked/sleeping/zombie), process table (`process/process.h`)
- [x] Context switching: register save/restore on the kernel stack, TSS.esp0 update
  per switch (`arch/x86/context.S`, `process/scheduler.c`)
- [x] Multilevel round-robin scheduler with anti-starvation aging, idle process
  (`process/scheduler.c`)
- [x] Switch page directories per process — every user process gets its own
  page directory (`vmm_create_directory`/`vmm_switch_directory`), switched on
  every context switch (`process/scheduler.c`)

## Phase 5: System Calls — Partially Complete
- [x] Ring 3 segments in GDT, TSS-based kernel-stack switching, privilege level changes
- [x] `int 0x80` syscall gate and dispatcher (`arch/x86/syscall.S`, `kernel/syscall.c`)
- [x] `exit()`, `write()`, `getpid()`, `fork()`, `yield()`, `sleep()`, `getppid()`, `wait()`
- [x] `read()` from a syscall — reuses the existing blocking `keyboard_get_line()`
  (`kernel/syscall.c` `sys_read`, `include/usyscall.h` `u_read`); demoed by the
  `uecho` shell command. There is still only one shared input line buffer, so a
  process reading concurrently with the shell's own prompt can race it — no
  per-process input queue/tty layer yet
- [x] **Fixed: ring-3 execution crashed.** Root cause: `vmm_init()` loaded CR3 but never
  set CR0.PG, so paging was never on. Per-process user stacks live at virtual
  `0x08000000`, which without paging is a raw physical address beyond the VM's RAM;
  ring-3 code read garbage back from its own stack and returned to EIP 0 / a small
  value (hence Invalid Opcode). Fix (`memory/vmm.c`): enable CR0.PG|WP. Two follow-on
  bugs fixed at the same time: page directories (2 pages) are now allocated with a real
  contiguous allocator, `pmm_alloc_pages()` (`memory/pmm.c`), instead of two
  `pmm_alloc_page()` calls that failed once memory was fragmented, and are freed in full;
  and children of kernel threads such as the shell are auto-reaped
  (`process/scheduler.c`) instead of staying zombies forever.
- [ ] `exec()` — not implemented; user "processes" are kernel-linked function pointers

## Phase 6: Enhanced Shell — ✅ Complete
- [x] Command tokenizing, registry/lookup, argument parsing
- [x] `help`, `clear`, `echo`, `uptime`, `meminfo`, `ps`, `kill`, `reboot`, and more
- [ ] Command history (up/down arrow navigation) — not implemented

## Phase 7: File System — Partially Complete
- [x] In-memory VFS (ramfs): directories, files, read/write, `ls`/`cat`/`stat`/`mkdir`/`touch`/`write`/`rm`
- [ ] Disk driver (ATA/IDE PIO) — not implemented, no persistent storage
- [ ] On-disk file system (FAT12/16 or custom) — not implemented

## Phase 8: Advanced Features — Partially Complete
- [x] IPC: pipes and message queues (`kernel/ipc.c`)
- [x] Multi-core detection via CPUID (`kernel/smp.c`)
- [x] Simulated in-memory GUI framebuffer with window management (`kernel/gui.c`)
- [ ] Networking: structures exist (Ethernet/IP/socket headers) but packet TX/RX and
  socket send/receive are stubs (`kernel/network.c`)
- [ ] SMP: CPU count detection works; Application Processor boot-up via APIC does not
- [ ] Real VESA/VBE graphics output (current GUI is an in-memory buffer, not displayed)
- [ ] Shell pipe redirection between commands (`kernel/script.c` has a stub)

## Phase 2 successor: Address Space Isolation — ✅ Complete

**Goal**: Close the memory-protection gap `memory/vmm.c` used to document — every
process shared one identity-mapped, fully `PTE_USER` address space, so there was no
isolation between processes or between user and kernel memory.

- [x] Per-process page directories with a private, non-shared physical backing for
  each process's user stack (`vmm_create_directory`/`vmm_switch_directory`,
  `process_create_user`, `sys_fork`)
- [x] Kernel/user memory protection: the identity map is supervisor-only by default;
  only `[kernel_start, rodata_start)` (the kernel's `.text`, needed since ring 3
  "processes" are kernel-linked functions with no `exec()` yet) stays `PTE_USER`.
  Kernel heap, the PMM bitmap, the process table (PCBs), and every process's kernel
  stack are no longer reachable from ring 3 (`memory/vmm.c`, `linker.ld`)
- [x] Any exception (page fault, divide-by-zero, invalid opcode, GPF, ...) that
  happens while running in ring 3 kills only the faulting process instead of
  halting the whole system, so the protection above is actually usable rather
  than freezing the OS on the first violation (`arch/x86/exceptions.c`);
  exceptions with CS RPL 0 (real kernel bugs) still panic and halt as before

**Residual scope** (not done here — separate, larger work):
- `.text` stays user-writable, not just user-executable — full W^X needs `exec()`
  so ring-3 code isn't kernel-linked into the same image
- Paging is now really enabled, so these mappings are enforced by the MMU

## Contributing to the Roadmap

Want to implement a feature? Great!

1. **Pick a task** from an unchecked item above
2. **Discuss your approach** (open an issue)
3. **Implement incrementally** (small PRs are better)
4. **Test thoroughly** (especially with QEMU)
5. **Document your changes** (code comments + docs) — and update this roadmap's
   checkboxes so it doesn't drift out of sync with the code again

## Learning Resources

### General
- [OSDev Wiki](https://wiki.osdev.org/) - Start here
- [The little book about OS development](https://littleosbook.github.io/)
- [xv6 Source Code](https://github.com/mit-pdos/xv6-public) - Great reference

### Topic-Specific
- **Interrupts**: [OSDev Interrupts](https://wiki.osdev.org/Interrupts)
- **Memory/Paging**: [OSDev Paging](https://wiki.osdev.org/Paging)
- **Processes**: [OSDev Kernel Multitasking](https://wiki.osdev.org/Kernel_Multitasking), [OSDev Getting to Ring 3](https://wiki.osdev.org/Getting_to_Ring_3)
- **Syscalls**: [OSDev System Calls](https://wiki.osdev.org/System_Calls)
- **Filesystems**: [OSDev VFS](https://wiki.osdev.org/VFS), [OSDev Ext2](https://wiki.osdev.org/Ext2)

---

This roadmap is a living document — please update the checkboxes above alongside
any code change that finishes or starts an item, rather than leaving it to drift.
