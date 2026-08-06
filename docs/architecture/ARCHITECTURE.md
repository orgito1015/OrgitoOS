# OrgitoOS - Modular Monolithic Architecture

## Overview

OrgitoOS has been refactored from a flat directory structure into a clean, modular monolithic architecture. This document describes the new architecture, design principles, and module organization.

## Architecture Philosophy

OrgitoOS follows a **monolithic kernel architecture** with strong modularity:

- **Everything runs in kernel space** - No microkernel-style IPC overhead
- **Clear module boundaries** - Well-defined interfaces between subsystems
- **No circular dependencies** - Unidirectional dependency flow
- **UNIX principles** - "Everything is a file" philosophy for future expansion

## Directory Structure

```
OrgitoOS/
├── arch/                   # Architecture-specific code
│   └── x86/               # x86-specific implementations
│       ├── boot.S         # Multiboot entry point, stack setup
│       ├── gdt.c/h        # Global Descriptor Table + TSS (ring 0/3 segments)
│       ├── idt.c/h        # Interrupt Descriptor Table
│       ├── isr.S/h        # Interrupt Service Routines
│       ├── pic.c/h        # Programmable Interrupt Controller
│       ├── ports.h        # x86 port I/O operations (inb/outb)
│       ├── exceptions.c/h/S  # CPU exception handlers
│       ├── context.S      # Context switch, enter_user_mode, fork_child_return
│       └── syscall.S      # int 0x80 entry stub
│
├── kernel/                # Core kernel subsystems
│   ├── kernel.c/h         # Main kernel entry and initialization
│   ├── panic.c/h          # Kernel panic handler
│   ├── syscall.c/h        # Syscall dispatcher (exit/write/getpid/fork/yield/sleep/getppid/wait)
│   ├── shell.c/h, commands.c/h, proc_commands.c  # Interactive shell and its commands
│   ├── string.c/h         # Freestanding string/memory helpers
│   ├── user_programs.c/h  # Demo ring-3 userland programs
│   ├── ipc.c, smp.c, gui.c, network.c, script.c  # See docs/architecture/ADVANCED_FEATURES.md
│   └── cpu/               # CPU pipeline/single-cycle/performance simulator (benchmarks only)
│
├── memory/                 # Memory management
│   ├── pmm.c/h              # Physical Memory Manager (bitmap page allocator)
│   ├── vmm.c/h              # Virtual Memory Manager (two-level paging)
│   ├── heap.c/h             # Kernel heap allocator (kmalloc/kfree, first-fit free list)
│   ├── slab.c/h             # Minimal slab-style free-list allocator (not currently used by process/scheduler)
│   ├── cache.c/h, bus.c/h   # CPU cache/bus models for the simulator, not the real kernel path
│
├── drivers/                # Hardware drivers
│   ├── console.c/h         # VGA text mode console (mirrors to serial)
│   ├── keyboard.c/h        # PS/2 keyboard driver
│   ├── timer.c/h           # Programmable Interval Timer (PIT)
│   └── serial.c/h          # 16550 UART (COM1) console mirror
│
├── fs/                     # File system support
│   ├── vfs.c/h             # In-memory VFS (ramfs): directories, files, read/write
│
├── process/                # Process management
│   ├── process.c/h         # PCBs, kernel threads, ring-3 processes, exit/kill/wait/sleep
│   └── scheduler.c/h       # Multilevel round-robin scheduler with aging
│
├── include/                # Common headers
│   ├── types.h, multiboot.h, usyscall.h   # Core type/ABI definitions
│   └── ipc.h, smp.h, gui.h, network.h, script.h  # Advanced-feature headers
│
├── build/                  # Build output (created by `make`)
│   ├── orgitoos.bin        # Final linked kernel binary
│   └── obj/                # Compiled object files (mirrors source tree)
│
├── Makefile               # Build system
├── linker.ld              # Linker script
└── grub.cfg               # GRUB bootloader configuration
```

## Module Dependencies

The kernel maintains a strict dependency hierarchy to prevent circular dependencies:

```
┌─────────────────────────────────────────┐
│           User Space (future)           │
└─────────────────────────────────────────┘
                   ▲
                   │ syscalls
┌─────────────────────────────────────────┐
│         Core Kernel (kernel/)           │
│   - kernel.c (initialization)           │
│   - panic.c (error handling)            │
└─────────────────────────────────────────┘
         ▲              ▲              ▲
         │              │              │
┌────────┴─────┐ ┌─────┴──────┐ ┌────┴─────┐
│   Drivers    │ │   Memory   │ │   FS     │
│   (drivers/) │ │  (memory/) │ │  (fs/)   │
│ - console    │ │ - pmm      │ │ - vfs    │
│ - keyboard   │ │ - vmm      │ │          │
│ - timer      │ │ - heap     │ │          │
└──────────────┘ └────────────┘ └──────────┘
         ▲              ▲              ▲
         │              │              │
         └──────────────┴──────────────┘
                   │
         ┌─────────┴──────────┐
         │   Architecture     │
         │   (arch/x86/)      │
         │ - boot.S           │
         │ - idt              │
         │ - isr              │
         │ - pic              │
         │ - exceptions       │
         │ - ports            │
         └────────────────────┘
                   │
         ┌─────────┴──────────┐
         │   Common Headers   │
         │   (include/)       │
         │ - types.h          │
         │ - multiboot.h      │
         └────────────────────┘
```

### Dependency Rules

1. **Upward dependencies only**: Lower layers don't depend on upper layers
2. **Common headers**: All modules can include types.h and multiboot.h
3. **Architecture isolation**: Only arch/ directly uses hardware-specific code
4. **Driver independence**: Drivers don't depend on each other
5. **Kernel coordination**: kernel/ coordinates between all subsystems

## Module Descriptions

### Architecture Layer (arch/x86/)

**Purpose**: Hardware abstraction and x86-specific operations

**Key Components**:
- `boot.S`: Multiboot entry, initial stack setup, calls `kmain()`
- `idt.c/h`: Interrupt Descriptor Table management
- `isr.S/h`: Low-level interrupt service routine stubs
- `pic.c/h`: 8259 PIC initialization and IRQ handling
- `ports.h`: Inline functions for x86 I/O port operations
- `exceptions.c/h/S`: CPU exception handlers with detailed error reporting

**Responsibilities**:
- Boot process initialization
- Interrupt and exception handling setup
- Hardware I/O abstractions
- CPU-specific operations

### Core Kernel (kernel/)

**Purpose**: System initialization and core kernel services

**Key Components**:
- `kernel.c/h`: Main entry point (`kmain`), subsystem initialization
- `panic.c/h`: Unrecoverable error handling

**Responsibilities**:
- Coordinate subsystem initialization
- Main kernel loop/scheduler (future)
- System call handling (future)
- Kernel panic and crash reporting

### Memory Management (memory/)

**Purpose**: Physical and virtual memory management

**Key Components**:
- `pmm.c/h`: Physical Memory Manager - bitmap-based page frame allocator
- `vmm.c/h`: Virtual Memory Manager - paging, page tables
- `heap.h`: Kernel heap allocator (future: kmalloc/kfree)

**Responsibilities**:
- Physical page allocation/deallocation
- Virtual memory mapping
- Page fault handling
- Dynamic memory allocation (future)

### Drivers (drivers/)

**Purpose**: Hardware device drivers

**Key Components**:
- `console.c/h`: VGA text mode output (80x25, scrolling)
- `keyboard.c/h`: PS/2 keyboard input with scancode translation
- `timer.c/h`: PIT timer for scheduling ticks

**Responsibilities**:
- Device initialization
- Interrupt handling for devices
- Device-specific I/O operations
- Buffer management

### File System (fs/)

**Purpose**: File system abstractions

**Key Components**:
- `vfs.c/h`: In-memory VFS (ramfs) — directories, files, read/write, seeded with `/etc/motd.txt`

**Responsibilities**:
- Unified file operations interface (`ls`, `cat`, `stat`, `mkdir`, `touch`, `write`, `rm`)
- Directory tree management (no separate mount-point layer yet — single ramfs root)

**Not yet implemented**: a disk-backed file system and driver (see
[roadmap.md](../roadmap/roadmap.md) Phase 7).

### Process Management (process/)

**Purpose**: Process and thread management

**Key Components**:
- `process.h/c`: Process control blocks, kernel threads, ring-3 user processes,
  fork/exit/wait/kill/sleep, process table (64 slots)
- `scheduler.h/c`: Multilevel round-robin scheduler (3 priority levels) with
  anti-starvation aging and a 50ms quantum

**Responsibilities**:
- Process creation and termination
- Context switching (`arch/x86/context.S`)
- Process scheduling, sleep/wake, zombie reaping

**Not yet implemented**: per-process address space isolation — see
[roadmap.md](../roadmap/roadmap.md)'s "Address Space Isolation" section.
- Thread management

### Common Headers (include/)

**Purpose**: Shared type definitions and structures

**Key Components**:
- `types.h`: Standard C types (uint32_t, size_t, etc.)
- `multiboot.h`: Multiboot specification structures

**Responsibilities**:
- Provide common type definitions
- Define cross-module structures
- Ensure type consistency

## Design Patterns

### 1. Initialization Pattern

All subsystems follow a consistent initialization pattern:

```c
void subsystem_init(void) {
    // 1. Initialize internal state
    // 2. Configure hardware (if applicable)
    // 3. Register interrupt handlers (if applicable)
    // 4. Enable operations
}
```

### 2. Interface Segregation

Each module exposes a minimal public interface:

```c
// In header file: Public interface
void driver_init(void);
void driver_read(void *buffer, size_t size);
void driver_write(const void *buffer, size_t size);

// In C file: Internal implementation
static void internal_helper(void);
static int internal_state;
```

### 3. Hardware Abstraction

Hardware access is isolated to specific modules:

```
Application Code
       ↓
   Driver API (console_write)
       ↓
   Driver Implementation (drivers/console.c)
       ↓
   Port I/O (arch/x86/ports.h)
       ↓
   Hardware
```

## Naming Conventions

### Files

- C source files: `module_name.c`
- C header files: `module_name.h`
- Assembly files: `module_name.S` (capital S for preprocessor)

### Functions

- Public functions: `module_function_name()`
  - Example: `console_write()`, `pmm_alloc_page()`
- Static/internal functions: `internal_function_name()`
  - Example: `terminal_scroll()`, `bitmap_set_bit()`

### Header Guards

- Format: `ORGITOOS_PATH_MODULE_H`
- Examples:
  - `ORGITOOS_ARCH_X86_IDT_H`
  - `ORGITOOS_DRIVERS_CONSOLE_H`
  - `ORGITOOS_MEMORY_PMM_H`

### Constants

- All caps with underscores: `CONSTANT_NAME`
- Module prefix for clarity: `MODULE_CONSTANT`
- Example: `VGA_WIDTH`, `PMM_PAGE_SIZE`, `PIC_EOI`

## Build System

### Makefile Organization

The Makefile is organized by directory:

```makefile
# Architecture objects
ARCH_OBJS = arch/x86/boot.o arch/x86/idt.o ...

# Kernel objects
KERNEL_OBJS = kernel/kernel.o kernel/panic.o

# Memory objects
MEMORY_OBJS = memory/pmm.o memory/vmm.o

# Driver objects
DRIVERS_OBJS = drivers/console.o drivers/keyboard.o ...
```

### Include Paths

The build system provides include paths for all major directories:

```makefile
CFLAGS += -I./include        # Common headers
CFLAGS += -I./arch/x86       # Architecture headers
CFLAGS += -I./kernel         # Kernel headers
CFLAGS += -I./memory         # Memory headers
CFLAGS += -I./drivers        # Driver headers
```

### Build Targets

- `make all`: Build the kernel binary
- `make clean`: Remove build artifacts
- `make run`: Build and run in QEMU (via ISO)
- `make iso`: Create bootable ISO image
- `make run-vbox`: Build and run in VirtualBox
- `make help`: Show available targets

## Future Extensions

### System Calls

System calls will be added in the kernel/ directory:

```
kernel/
├── syscall.c/h          # System call dispatcher
├── syscall_table.c      # System call table
└── syscall_handlers.c   # System call implementations
```

### Scheduling

Process scheduling will be added in kernel/ and process/:

```
kernel/
└── scheduler.c/h        # Scheduler implementation

process/
├── process.c            # Process management
└── context.S            # Context switching
```

### Virtual File System

VFS will be fully implemented in fs/:

```
fs/
├── vfs.c/h              # VFS core
├── ramfs.c/h            # RAM-based FS
└── devfs.c/h            # Device file system
```

## Benefits of This Architecture

### 1. Simplicity

- **Clear structure**: Each directory has a specific purpose
- **Easy navigation**: Find code quickly by functionality
- **Self-documenting**: Directory names explain module purposes

### 2. Performance

- **Monolithic design**: No IPC overhead between kernel subsystems
- **Direct function calls**: Fast communication within kernel
- **Efficient memory access**: All kernel code in same address space

### 3. Control

- **Fine-grained control**: Direct hardware access where needed
- **Flexible design**: Easy to add new features
- **No abstraction overhead**: Direct control when necessary

### 4. Learning Clarity

- **Modular organization**: Learn one subsystem at a time
- **Clear dependencies**: Understand system relationships
- **Progressive complexity**: Start simple, add features incrementally

### 5. Expandability

- **Plugin architecture**: Easy to add new drivers
- **Scalable design**: Structure supports growth
- **Extension points**: Clear places to add functionality
- **Backward compatibility**: Changes are isolated to modules

## Comparison: Before vs After

### Before (Flat Structure)

```
Kernel2.0/
├── boot.S
├── kernel.c
├── idt.c/h
├── pic.c/h
├── isr.S/h
├── exceptions.c/h/S
├── pmm.c/h
├── vmm.c/h
├── keyboard.c/h
├── timer.c/h
└── linker.ld
```

**Issues**:
- All files mixed together
- Hard to understand relationships
- Difficult to find specific functionality
- No clear separation of concerns

### After (Modular Structure)

```
├── arch/x86/          # Hardware abstraction
├── kernel/            # Core kernel
├── memory/            # Memory management
├── drivers/           # Device drivers
├── fs/                # File systems
├── process/           # Process management
└── include/           # Common headers
```

**Benefits**:
- Clear separation of concerns
- Easy to navigate and understand
- Scalable for future growth
- Industry-standard organization

## Conclusion

This modular monolithic architecture provides OrgitoOS with a solid foundation for growth while maintaining the performance benefits of a monolithic kernel. The clear structure makes it easier for developers to understand, maintain, and extend the system.

## References

- Linux kernel architecture
- xv6 educational operating system
- Modern Operating Systems (Tanenbaum)

### OSDev Wiki

- [Expanded Main Page](https://wiki.osdev.org/Expanded_Main_Page) - topic index for everything below
- [Meaty Skeleton](https://wiki.osdev.org/Meaty_Skeleton) - the build/boot layout this project's Makefile and `arch/x86/boot.S` are modeled on
- [Paging](https://wiki.osdev.org/Paging) / [Memory Management](https://wiki.osdev.org/Memory_Management) - background for `memory/pmm.c` and `memory/vmm.c`
- [Kernel Multitasking](https://wiki.osdev.org/Kernel_Multitasking) / [Getting to Ring 3](https://wiki.osdev.org/Getting_to_Ring_3) - background for `process/` and the GDT/TSS setup in `arch/x86/gdt.c`
