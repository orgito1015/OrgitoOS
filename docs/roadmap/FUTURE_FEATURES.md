# OrgitoOS — Future Features Roadmap

## Context

`docs/roadmap/roadmap.md` is now fully complete — every phase through per-CPU
SMP scheduling, real graphics, minimal TCP, and a higher-half kernel is
checked off and pushed. `docs/roadmap/OS_EVOLUTION_STRATEGY.md` is an older,
much more sprawling wishlist written when the kernel had almost nothing;
most of its Section 1 (memory management, exceptions, process management,
scheduler, syscalls) and Section 3.1 (SMP) are now done, which that
document itself doesn't reflect since it hasn't been touched since. This
file replaces it as the actual forward-looking plan: a curated, prioritized
set of what's genuinely next, informed by what actually exists in the code
today rather than a from-scratch wishlist.

Nothing here has been started. Pick a tier (or a single item) and say so —
this file is meant to be discussed before any of it gets built, not treated
as a queue to work through unattended.

---

## Tier 1 — Debugging infrastructure (recommended starting point)

**Why first**: everything built so far was verified by static analysis and
careful reasoning because no boot-testing tool was available in this
environment. A GDB stub removes that constraint going forward — it lets
*you* (with real QEMU) actually single-step, inspect memory, and set
breakpoints in a live kernel, which is a bigger unlock for every other item
below than any single feature would be.

- **GDB remote stub over serial** — implement the GDB Remote Serial
  Protocol (RSP) on top of `drivers/serial.c`'s existing COM1 support.
  `Makefile`'s `make debug`/`make gdb` targets already start QEMU with `-s
  -S` and attach GDB, but that only works today because QEMU's own
  built-in stub is doing the work pre-boot; a stub *inside* the kernel
  would let you break into a running, already-booted OrgitoOS the same
  way. Minimal viable version: handle `g`/`G` (read/write registers),
  `m`/`M` (read/write memory), `c`/`s` (continue/step), `Z`/`z`
  (breakpoints via `int3` patching). Entry point: a dedicated interrupt
  vector or a magic keystroke combo that traps into the stub.
- **Kernel panic backtraces** — `kernel/panic.c` and
  `arch/x86/exceptions.c` already print register dumps; walking the
  `%ebp` chain (this kernel is built without `-fomit-frame-pointer`
  specifically so this works) to print a call stack on panic/exception
  would make every future crash dramatically easier to diagnose without
  a debugger at all.
- **`make test`: headless boot-and-assert harness** — script QEMU with
  `-serial stdio -display none`, feed a sequence of shell commands via a
  simulated keyboard or a `-append`-style boot parameter, capture serial
  output, and grep for expected strings (`"System Ready"`,
  `"PID 0 = idle"`, etc.). Doesn't need real GDB — just proves the kernel
  boots and the shell responds, which is exactly the class of thing that
  couldn't be verified this session. Needs `qemu-system-i386` installed
  (declined earlier, but this would be the concrete payoff for revisiting
  that).

## Tier 2 — Security hardening

Directly continues `OS_EVOLUTION_STRATEGY.md` Section 2, now scoped
against what's actually true of this kernel today.

- **Stack canaries** — `Makefile`'s `CFLAGS` explicitly has
  `-fno-stack-protector`. Flip it on, implement `__stack_chk_fail()`
  (currently absent — GCC would emit calls to it that fail to link), and
  seed a per-boot (or per-process) random canary. Low-risk, no
  architecture changes, real security value.
- **Close the residual W^X gap** — `docs/roadmap/roadmap.md`'s Address
  Space Isolation section already documents this precisely: kernel-linked
  ring-3 demo functions (`kernel/user_programs.c`) still share the
  kernel's own `.text` pages (necessarily writable at the page level,
  since ring-3 fetches from the same mapping the kernel writes its own
  code through at link time) instead of getting a private, read-only-once-
  loaded mapping the way `exec()`'d binaries already do
  (`kernel/exec.c` sets `PTE_WRITABLE` only when `PF_W` is set in the
  ELF program header). Fix: give the demo entries the same treatment, or
  retire them in favor of exec()'d equivalents now that exec() exists.
- **Real NX/DEP enforcement** — honest scope note: 32-bit x86 *without*
  PAE has no per-page execute-disable bit at all; the NX bit only exists
  under PAE or long mode. This item is genuinely blocked on Tier 4's PAE
  or x86-64 migration, not implementable standalone — listed here for
  completeness, not as something to start on its own.
- **ASLR (partial)** — full KASLR needs position-independent kernel
  relocation (large, invasive). A much smaller, real first step: randomize
  each process's user-stack placement within a safe window instead of the
  fixed `PROCESS_USTACK_VIRT_BASE` every process uses today, and randomize
  `exec()`'d binaries' load bias.

## Tier 3 — Finish what TCP deliberately left out

`kernel/network.c`'s `handle_tcp()` is explicitly scoped as client-only,
no retransmission, no congestion control (see `tcp_state_t`'s doc comment
in `include/network.h`). Natural, bounded continuations, roughly in order
of value:

- **Retransmission on packet loss** — a per-socket retransmit timer and
  one buffered unacked segment (this stack sends one segment per
  `net_socket_send()` call already, so "one" is enough for a first cut);
  resend if no ACK within a timeout, give up after N tries.
- **`listen()`/`accept()`** — passive-open support, adding `TCP_LISTEN`
  and `TCP_SYN_RCVD` to `tcp_state_t`, and a backlog of pending
  connections per listening socket. Lets OrgitoOS act as a TCP *server*,
  not just a client — meaningfully more state-machine surface than the
  client-only handshake already built.
- **DHCP client** — `net_init()` currently hardcodes
  `10.0.2.15/24`/gateway `10.0.2.2` (QEMU SLIRP defaults). A real DHCP
  DISCOVER/OFFER/REQUEST/ACK exchange over the existing UDP socket API
  would let OrgitoOS configure itself on a real network instead.
- **DNS resolution** — a `dns_resolve(hostname) -> ip_addr_t` built on
  the same UDP socket primitives, so `ping`/`tcpconnect` could take
  hostnames instead of only dotted-decimal addresses.

## Tier 4 — Architecture: PAE or x86-64

The single biggest lever available, and the one everything in Tier 2's NX
item and much of `OS_EVOLUTION_STRATEGY.md` Section 2 is actually blocked
on. Two paths, not both:

- **PAE (stay 32-bit)** — smaller step: enables the NX bit and >4 GiB
  physical addressing without a full architecture rewrite. Still touches
  `memory/vmm.c`'s page table format (3-level PAE tables instead of the
  current 2-level), `arch/x86/boot.S`'s CR4.PAE enable, and every
  PTE-flag-touching call site.
- **x86-64 (go 64-bit)** — a genuinely large, cross-cutting change (every
  pointer-to-physical assumption in the codebase changes width and the
  page table format changes from 2-level to 4-level), but graduates
  OrgitoOS to a real 64-bit kernel: more registers, real NX without PAE's
  compromises, a much larger address space. `OS_EVOLUTION_STRATEGY.md`
  §11.1 already recommends this path over staying 32-bit long-term.
  **Not a small follow-on task** — this is its own multi-phase project on
  the scale of the 7 phases just finished, not something to fold into a
  quick session.

## Tier 5 — Process/userland maturity

- **POSIX-ish signals** — `process_t` has no signal-related fields at
  all today; `int 0x80`'s syscall table (`kernel/syscall.h`) has no
  `SYS_KILL`-with-signal-number, `SYS_SIGACTION`, or `SYS_SIGRETURN`.
  Real fault-to-signal delivery (e.g. turning a ring-3 page fault into a
  deliverable `SIGSEGV` instead of just killing the process outright,
  building on the per-process fault handling `arch/x86/exceptions.c`
  already has) would be the most natural entry point.
- **A minimal libc for userland** — `userland/hello.c` and its
  `include/usyscall.h` wrappers are the entire userland API surface today
  (raw syscall stubs, no `printf`, no `malloc`). A small freestanding
  libc (string functions, a bump/free-list `malloc` over `sys_brk`-style
  growth, basic stdio over `write()`) would make it practical to port
  simple real C programs instead of hand-writing everything against raw
  syscalls.

## Tier 6 — GUI/input

- **PS/2 mouse driver** — `drivers/keyboard.c` already owns the 8042
  controller programming pattern to extend; a second IRQ (IRQ12) and a
  3-byte packet parser would give `kernel/gui.c` real cursor input, which
  it has no source of today (`gui_flip_to_screen()` is draw-only).
- **Continuous compositor** — today's GUI flip is one-shot (a shell
  command renders once and blits). A redraw loop — even a simple "redraw
  on tick" driven off the existing scheduler — would make the windowing
  system feel like an actual live UI instead of a snapshot demo.

---

## Recommendation

Start with **Tier 1's GDB stub** if you want the highest-leverage single
addition — it changes every *future* session's risk profile, not just
adding one more feature. If you'd rather see visible new capability first,
**Tier 3 (TCP retransmission + listen/accept)** is the most contained,
lowest-risk continuation of code that already exists and already works.
Tier 4 (PAE/x86-64) is listed for completeness but shouldn't be started
casually — treat it as its own future planning session, not a quick add-on.
