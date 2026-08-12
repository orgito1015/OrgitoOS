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
  (`ls`, `cat`, `stat`, `mkdir`, `touch`, `write`, `rm`), plus a real ATA/IDE PIO disk
  driver and a custom on-disk filesystem ("OrgitoFS") mounted at `/mnt` so files
  persist across reboots
- IPC (pipes, message queues), real APIC-driven SMP bring-up with a genuine
  per-CPU scheduler (own ready queues, own TSS, own LAPIC-timer preemption,
  work-stealing load balancing), a simulated
  in-memory GUI framebuffer, a real Ethernet/ARP/IP/ICMP/UDP stack (plus a
  minimal client-only TCP state machine) over a PCI-enumerated RTL8139 NIC,
  shell scripting with variables, shell pipe redirection (`cmd1 | cmd2`),
  `fork()` that works after `exec()`, command history (up/down arrow), and
  `exec()` of real ELF32 binaries

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
- [x] Higher-half kernel mapping — the kernel now actually runs from
  `KERNEL_VIRTUAL_BASE` (0xC0000000), dual-mapped rather than pure
  higher-half (see `memory/vmm.c` `vmm_init()`'s design-decision comment
  for why: the codebase pervasively treats PMM-allocated physical
  addresses as directly dereferenceable pointers, and keeping the
  existing full low-RAM identity map completely intact - just *adding*
  a higher-half alias for the kernel image on top of it - satisfies the
  literal ask while touching the minimum possible surface elsewhere).
  `linker.ld` links every section but a small `.text.boot`/`.bss.boot`
  pre-paging bootstrap (`arch/x86/boot.S`) at their higher-half VMA while
  their LMA stays physical; `_start` builds a temporary 64 MiB PSE
  identity+high-alias mapping, enables paging, and jumps into the
  higher-half-linked `higher_half_entry` before `vmm_init()` later builds
  the permanent mapping. Verified statically (no boot testing was in
  scope) via `readelf -l`: the ELF's second `LOAD` segment reports
  `VirtAddr=0xc0102000`/`PhysAddr=0x00102000`, and `e_entry` stays a low,
  physically-reachable address

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

## Phase 5: System Calls — ✅ Complete
- [x] Ring 3 segments in GDT, TSS-based kernel-stack switching, privilege level changes
- [x] `int 0x80` syscall gate and dispatcher (`arch/x86/syscall.S`, `kernel/syscall.c`)
- [x] `exit()`, `write()`, `getpid()`, `fork()`, `yield()`, `sleep()`, `getppid()`, `wait()`
- [x] `read()` from a syscall — reuses the existing blocking `keyboard_get_line()`
  (`kernel/syscall.c` `sys_read`, `include/usyscall.h` `u_read`); demoed by the
  `uecho` shell command. There is still only one shared input line buffer, so a
  process reading concurrently with the shell's own prompt can race it — no
  per-process input queue/tty layer yet
- [x] `exec()` — `SYS_EXEC`/`u_exec()` load a real, statically-linked ELF32
  binary from the VFS and spawn it as a new process (`kernel/exec.c`
  `process_exec`), parsing PT_LOAD segments into freshly allocated, privately
  mapped pages instead of jumping to a kernel-linked function pointer. Spawn-
  style rather than in-place image replacement — see `kernel/exec.c`'s header
  comment for why. A real demo binary (`userland/hello.c`, built and embedded
  by the Makefile's userland rules) is preloaded to `/bin/hello` at boot; try
  `exec /bin/hello`. `fork()` of an already-exec()'d process is now
  supported: `process_t.exec_pages` (`process/process.h`) records each
  PT_LOAD page's virtual address and mapping flags alongside its physical
  frame, so `sys_fork()` (`kernel/syscall.c`) deep-copies an exec()'d image
  into the child's own page directory at the identical virtual addresses,
  the same technique already used for the fixed user-stack window

## Phase 6: Enhanced Shell — ✅ Complete
- [x] Command tokenizing, registry/lookup, argument parsing
- [x] `help`, `clear`, `echo`, `uptime`, `meminfo`, `ps`, `kill`, `reboot`, and more
- [x] Command history (up/down arrow navigation) — `drivers/keyboard.c` keeps a
  16-entry ring buffer; Up/Down (scan codes `0x48`/`0x50`, previously silently
  dropped) recall and redraw a previous line in place. Lives in the one shared
  `keyboard_get_line()` input path, so it works identically for the shell
  prompt and any process's `read()`

## Phase 7: File System — ✅ Complete
- [x] In-memory VFS (ramfs): directories, files, read/write, `ls`/`cat`/`stat`/`mkdir`/`touch`/`write`/`rm`
- [x] Disk driver (ATA/IDE PIO) — `drivers/ata.c`: polled-PIO driver for the primary
  ATA bus's master drive (ports 0x1F0-0x1F7 + 0x3F6), IDENTIFY-based detection,
  28-bit LBA sector read/write. Safe no-op if no disk is attached.
- [x] On-disk file system (custom) — `fs/diskfs.c` ("OrgitoFS"): a small custom
  filesystem persisted to the ATA disk, mounted at `/mnt` alongside the in-memory
  ramfs at `/`. Fixed-size superblock + flat inode table + block bitmap, direct
  blocks only (no indirect blocks), formats a fresh filesystem automatically if no
  valid superblock is found. Reuses `fs/vfs.c`'s generic `vfs_add_child()`/
  `vfs_find_node()`/`vfs_remove_child()` for the in-memory node tree, so no
  on-disk directory-entry blocks are needed. Files created under `/mnt` survive a
  reboot, backed by `disk.img` (created automatically by the Makefile, 16 MiB,
  never touched by `make clean`).

## Phase 8: Advanced Features — ✅ Complete
- [x] IPC: pipes and message queues (`kernel/ipc.c`)
- [x] Multi-core detection via CPUID (`kernel/smp.c`)
- [x] Simulated in-memory GUI framebuffer with window management (`kernel/gui.c`),
  now presentable to a real display via `gui_flip_to_screen()` (`drivers/framebuffer.c`)
- [x] Networking: real PCI enumeration (`kernel/pci.c`) finds an RTL8139 NIC and a
  full I/O-port driver (`drivers/rtl8139.c`) drives its RX ring and TX descriptors
  for genuine packet TX/RX — no more stubs. `kernel/network.c` builds a real
  Ethernet/ARP/IP/ICMP/UDP stack on top: ARP request/reply with a cache
  (opportunistically warmed from any received IP packet's source MAC, which is
  also what keeps the IRQ-context ICMP echo-reply path from ever needing to
  block), a `ping` shell command (real ICMP echo request/reply round trip), and
  `net_socket_send`/`net_socket_recv` that actually build/parse UDP datagrams
  against a per-socket receive queue instead of returning `size` unchanged, and
  a minimal client-only TCP state machine (`handle_tcp()`): active-open
  `net_socket_connect()` does a real 3-way handshake (with checksum computed
  over the RFC 793 pseudo-header) and blocks for it, `net_socket_send()`/
  `net_socket_recv()` work over an established connection sharing the same
  per-socket receive queue UDP uses, and `net_socket_close()` sends FIN and
  walks FIN_WAIT1 → FIN_WAIT2 → CLOSED. No `listen()`/`accept()` (client-only),
  no retransmission on loss, no congestion control — a `tcpconnect <ip> <port>
  [message]` shell command demonstrates it. QEMU users: `-device rtl8139,netdev=n0 -netdev
  user,id=n0` (already wired into `make run`/`run-iso`/`debug`/`qemu-log` and
  `tools/run-qemu.sh`) — defaults match SLIRP's 10.0.2.15/24, gateway 10.0.2.2
- [x] SMP: real APIC-driven Application Processor boot-up. `kernel/apic.c` parses
  ACPI's RSDP/RSDT/MADT for real Local APIC IDs and the Local APIC's MMIO base;
  `kernel/smp.c` drives the INIT-SIPI-SIPI sequence; `arch/x86/trampoline.S` (a
  real-mode stub linked at a fixed physical address and copied there at boot,
  the same embedding technique as the userland ELF binary above) takes each AP
  from 16-bit real mode into 32-bit protected mode with paging on, sharing the
  BSP's own page directory, before handing off to `ap_entry()` in C. APs are
  brought up one at a time and, once online, are now fully schedulable: each
  gets its own TSS (`arch/x86/gdt.c` — one descriptor per possible CPU, so
  the old single shared TSS's "busy bit" hazard is gone), its own dedicated
  idle process (`process_init_ap()`), and its own preemption source — the
  Local APIC's built-in periodic timer, calibrated once against the PIT and
  armed on every AP (`kernel/apic.c` `lapic_timer_calibrate()`/
  `lapic_timer_start()`; legacy PIC IRQs are never routed to APs in this
  kernel, only the BSP still uses the PIT). `process/percpu.c` gives every
  CPU its own `current_process` and ready queues (`process/percpu.h`); the
  old single global scheduler state is gone. Work-stealing load balancing:
  when a CPU's own queues are empty it steals from the busiest other CPU
  before falling back to its own idle process (`process/scheduler.c`
  `try_steal_work()`). Every process-table and ready-queue mutation is now
  protected by one global spinlock (`g_sched_lock`) in addition to the
  existing `cli` sections — a coarse but provably deadlock-free design (see
  `process/percpu.h`'s file header for why per-CPU locks were considered and
  rejected)
- [x] Real VESA/VBE graphics output — `arch/x86/boot.S`'s Multiboot header now
  requests a linear graphics framebuffer (flag bit 2, 1024x768x32); GRUB/QEMU
  sets the mode up before jumping to the kernel, so no runtime BIOS/VM86 calls
  are needed. `drivers/framebuffer.c` provides a small 5x7 bitmap-font text
  renderer over real pixels plus a raw blit entry point
  (`fb_blit_argb_buffer()`), and `drivers/console.c` picks that backend at
  boot instead of the legacy VGA text buffer whenever `multiboot_info`
  reports a usable 32bpp RGB framebuffer — **falling back to the exact
  original VGA text path otherwise**, so an unsatisfied video-mode request
  degrades gracefully rather than producing a blank console. Every
  `console.c` function keeps its exact prior signature, so no caller
  anywhere in the kernel needed to change. `kernel/gui.c`'s existing
  simulated 800x600 framebuffer can now actually reach the screen via the
  new `gui_flip_to_screen()` (wired into `test_gui`'s demo)
- [x] Shell pipe redirection between commands — `cmd1 | cmd2 | ...` pipelines in
  `kernel/shell.c` capture each non-final stage's console output
  (`console_set_capture`, `drivers/console.c`) and feed it to the next stage as
  stdin (`shell_stdin_read_line`); `kernel/script.c`'s `script_create_pipe` now
  hands off to the same real pipeline runner instead of only printing an
  announcement. New pipe-aware filter commands: `grep <pattern>`, `wc`

## Phase 2 successor: Address Space Isolation — ✅ Complete

**Goal**: Close the memory-protection gap `memory/vmm.c` used to document — every
process shared one identity-mapped, fully `PTE_USER` address space, so there was no
isolation between processes or between user and kernel memory.

- [x] Per-process page directories with a private, non-shared physical backing for
  each process's user stack (`vmm_create_directory`/`vmm_switch_directory`,
  `process_create_user`, `sys_fork`)
- [x] Kernel/user memory protection: the identity map is supervisor-only by default;
  only `[kernel_start, rodata_start)` (the kernel's `.text`) stays `PTE_USER`, so
  ring 3 "processes" - both the kernel-linked demo entries
  (`kernel/user_programs.c`) and real `exec()`'d ELF32 binaries (`kernel/exec.c`)
  alike - can fetch their own code. Kernel heap, the PMM bitmap, the process
  table (PCBs), and every process's kernel stack are no longer reachable from
  ring 3 (`memory/vmm.c`, `linker.ld`). Since the higher-half move (this
  section's own entry above), that carve-out lives at the kernel's higher-half
  address (`kernel_start`/`rodata_start`, now virtual) mapped to its physical
  backing (`kernel_start_phys`/`rodata_start_phys`) via `vmm_map_region()`,
  rather than an identity mapping - the mechanism changed, the protection
  boundary didn't
- [x] Any exception (page fault, divide-by-zero, invalid opcode, GPF, ...) that
  happens while running in ring 3 kills only the faulting process instead of
  halting the whole system, so the protection above is actually usable rather
  than freezing the OS on the first violation (`arch/x86/exceptions.c`);
  exceptions with CS RPL 0 (real kernel bugs) still panic and halt as before

**Residual scope** (not done here — separate, larger work):
- `.text` stays user-writable, not just user-executable - full W^X needs
  loaded binaries to get their own private, non-kernel-linked mapping instead
  of sharing the kernel image's own pages the way the kernel-linked demo
  entries (`kernel/user_programs.c`) still do (`kernel/exec.c`'s ELF loader
  already maps each process's own PT_LOAD segments privately and correctly
  marks non-writable segments read-only - see its `flags` computation - so
  this gap is specifically about the older kernel-linked-function code path,
  not exec()'d binaries)

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
