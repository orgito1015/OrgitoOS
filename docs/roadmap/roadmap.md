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
  `getppid`/`wait`)
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
- [ ] Switch page directories per process — every process currently shares the
  kernel's single page directory (see "Phase 2 (address space isolation)" below)

## Phase 5: System Calls — ✅ Complete
- [x] Ring 3 segments in GDT, TSS-based kernel-stack switching, privilege level changes
- [x] `int 0x80` syscall gate and dispatcher (`arch/x86/syscall.S`, `kernel/syscall.c`)
- [x] `exit()`, `write()`, `getpid()`, `fork()`, `yield()`, `sleep()`, `getppid()`, `wait()`
- [ ] `read()` from a syscall (keyboard input is currently shell-only, not exposed to user processes)
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

## Next: Address Space Isolation (Phase 2 successor)

**Goal**: Close the memory-protection gap `memory/vmm.c` itself documents — every
process currently shares one identity-mapped, fully `PTE_USER` address space, so
there's no isolation between processes or between user and kernel memory.

- [ ] Per-process page directories with a private, non-shared physical backing for
  each process's user stack (activates `vmm_create_directory`/`vmm_switch_directory`,
  currently unused)
- [ ] Kernel/user memory protection: split the identity map at the code/data boundary
  so ring 3 can no longer read/write kernel heap, other processes' kernel stacks, or PCBs

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
