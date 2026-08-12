# OrgitoOS Kernel Makefile
# Builds the kernel binary from the modular directory structure

# Target binary name and output location
TARGET = orgitoos
OUTPUT_DIR = build
OBJDIR = $(OUTPUT_DIR)/obj
OUTPUT_BIN = $(OUTPUT_DIR)/$(TARGET).bin

# QEMU settings
QEMU     = qemu-system-i386
ISO_FILE = orgitoos.iso
# -device rtl8139: pin the emulated NIC explicitly rather than relying on
# QEMU's default (which varies by version/machine type) - drivers/rtl8139.c
# only knows how to drive this one chip. -netdev user gives it QEMU's
# built-in SLIRP network (guest 10.0.2.15/24, gateway 10.0.2.2), matching
# kernel/network.c's net_init() defaults - `ping 10.0.2.2` should work
# out of the box.
QEMU_NET_FLAGS = -netdev user,id=n0 -device rtl8139,netdev=n0
# Persistent data disk for drivers/ata.c + fs/diskfs.c (mounted at /mnt).
# A raw flat image, not touched by `make clean` - it's user data, exactly
# like a real hard disk, and surviving reboots/rebuilds is the entire
# point of it existing.
DISK_IMG    = disk.img
DISK_SIZE_MB = 16
QEMU_DISK_FLAGS = -hda $(DISK_IMG)
QEMU_FLAGS = -cdrom $(ISO_FILE) -boot d $(QEMU_NET_FLAGS) $(QEMU_DISK_FLAGS)

# Compiler selection: prefer cross-compiler, fallback to native gcc
CC := $(shell command -v i686-elf-gcc 2> /dev/null || echo gcc)
OBJCOPY := $(shell command -v i686-elf-objcopy 2> /dev/null || echo objcopy)

ifeq ($(CC),gcc)
$(warning No i686-elf-gcc cross-compiler found on PATH; falling back to host gcc -m32. \
This works on Linux/WSL (ELF-native host gcc) but will NOT work with a Windows-native \
MinGW gcc, which emits PE/COFF objects incompatible with this freestanding ELF build. \
See https://wiki.osdev.org/GCC_Cross-Compiler to build a proper cross toolchain.)
endif

# Compiler flags for freestanding kernel development
CFLAGS = -std=gnu99          # Use GNU C99 standard
CFLAGS += -ffreestanding     # Kernel environment (no hosted libs)
CFLAGS += -O2                # Optimization level 2
CFLAGS += -Wall -Wextra      # Enable all warnings
CFLAGS += -m32                # 32-bit x86 target
CFLAGS += -fno-pic           # No position-independent code
CFLAGS += -fno-stack-protector  # No stack canaries (not available in kernel)
CFLAGS += -fno-omit-frame-pointer  # Keep EBP chains (needed by fork() stack relocation)
CFLAGS += -nostdlib          # Don't link standard library
CFLAGS += -nostartfiles      # Don't use standard startup files
CFLAGS += -nodefaultlibs     # Don't use default libraries
CFLAGS += -I./include        # Include common headers
CFLAGS += -I./arch/x86       # Include x86 architecture headers
CFLAGS += -I./kernel         # Include kernel headers
CFLAGS += -I./memory         # Include memory headers
CFLAGS += -I./drivers        # Include driver headers
CFLAGS += -I./fs             # Include filesystem headers
CFLAGS += -I./process        # Include process management headers

# Assembly flags (same as C flags for consistency)
ASFLAGS = $(CFLAGS)

# Automatic header-dependency generation (covers transitively-included
# headers, e.g. include/types.h, that hand-written prerequisite lists miss).
DEPFLAGS = -MMD -MP

# Linker flags
LDFLAGS = -ffreestanding     # Freestanding environment
LDFLAGS += -nostdlib         # No standard library
LDFLAGS += -static           # Static linking only
LDFLAGS += -Wl,--nmagic      # Don't align data segments to page boundaries
LDFLAGS += -Wl,-z,noexecstack  # Mark stack as non-executable

# Source directories
ARCH_DIR = arch/x86
KERNEL_DIR = kernel
CPU_DIR = $(KERNEL_DIR)/cpu
MEMORY_DIR = memory
DRIVERS_DIR = drivers
FS_DIR = fs
PROCESS_DIR = process

# C sources (arch/x86/exceptions.S below shares a basename with
# arch/x86/exceptions.c, so its object gets an explicit rule/name further
# down instead of going through the generic pattern rule).
C_SOURCES = $(ARCH_DIR)/gdt.c $(ARCH_DIR)/idt.c $(ARCH_DIR)/pic.c $(ARCH_DIR)/irq.c $(ARCH_DIR)/exceptions.c \
            $(KERNEL_DIR)/kernel.c $(KERNEL_DIR)/syscall.c $(KERNEL_DIR)/user_programs.c \
            $(KERNEL_DIR)/proc_commands.c $(KERNEL_DIR)/panic.c $(KERNEL_DIR)/string.c \
            $(KERNEL_DIR)/shell.c $(KERNEL_DIR)/commands.c $(KERNEL_DIR)/ipc.c \
            $(KERNEL_DIR)/exec.c $(KERNEL_DIR)/pci.c \
            $(KERNEL_DIR)/smp.c $(KERNEL_DIR)/apic.c $(KERNEL_DIR)/gui.c $(KERNEL_DIR)/network.c $(KERNEL_DIR)/script.c \
            $(CPU_DIR)/pipeline.c $(CPU_DIR)/single_cycle.c $(CPU_DIR)/performance.c \
            $(MEMORY_DIR)/pmm.c $(MEMORY_DIR)/vmm.c $(MEMORY_DIR)/heap.c \
            $(MEMORY_DIR)/cache.c $(MEMORY_DIR)/bus.c \
            $(DRIVERS_DIR)/console.c $(DRIVERS_DIR)/serial.c $(DRIVERS_DIR)/keyboard.c $(DRIVERS_DIR)/timer.c \
            $(DRIVERS_DIR)/rtl8139.c $(DRIVERS_DIR)/ata.c $(DRIVERS_DIR)/framebuffer.c \
            $(FS_DIR)/vfs.c $(FS_DIR)/diskfs.c \
            $(PROCESS_DIR)/process.c $(PROCESS_DIR)/scheduler.c $(PROCESS_DIR)/percpu.c

# Assembly sources (excluding exceptions.S, handled separately above)
ASM_SOURCES = $(ARCH_DIR)/boot.S $(ARCH_DIR)/isr.S $(ARCH_DIR)/context.S $(ARCH_DIR)/syscall.S

# Object files: mirror the source tree under build/obj/
C_OBJS   = $(patsubst %.c,$(OBJDIR)/%.o,$(C_SOURCES))
ASM_OBJS = $(patsubst %.S,$(OBJDIR)/%.o,$(ASM_SOURCES))
EXCEPTIONS_ASM_OBJ = $(OBJDIR)/$(ARCH_DIR)/exceptions_asm.o

USERLAND_DIR = userland
USERLAND_OBJDIR = $(OBJDIR)/userland
USERLAND_EMBED_OBJ = $(USERLAND_OBJDIR)/hello_embed.o

TRAMPOLINE_OBJDIR = $(OBJDIR)/$(ARCH_DIR)
TRAMPOLINE_EMBED_OBJ = $(TRAMPOLINE_OBJDIR)/trampoline_embed.o

OBJS = $(C_OBJS) $(ASM_OBJS) $(EXCEPTIONS_ASM_OBJ) $(USERLAND_EMBED_OBJ) $(TRAMPOLINE_EMBED_OBJ)

# Default target: build the kernel
all: $(OUTPUT_BIN)

# Generic pattern rules: any source file compiles to its mirrored path
# under build/obj/, with automatic header-dependency tracking.
$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) $(DEPFLAGS) -c $< -o $@

# arch/x86/exceptions.S shares a basename with arch/x86/exceptions.c, so it
# needs an explicit rule to avoid colliding with that object's pattern match.
$(EXCEPTIONS_ASM_OBJ): $(ARCH_DIR)/exceptions.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) $(DEPFLAGS) -c $< -o $@

# ---------------------------------------------------------------------
# Userland demo binary (userland/hello.c), embedded into the kernel.
#
# Built as its own freestanding ELF32 executable (userland/user.ld),
# completely separate from the kernel image/linker.ld, then objcopy'd
# into a plain byte blob and linked into the kernel as
# _binary_hello_elf_{start,end,size} (kernel/exec.c reads these to
# preload /bin/hello at boot). This is what makes `exec /bin/hello` load
# a real compiled-and-linked binary instead of a kernel-linked function
# pointer (kernel/user_programs.c's demo programs).
#
# These rules must stay below the `all` target above: GNU Make's default
# goal is the first rule textually in the file, and these are pulled in
# as a prerequisite of $(OUTPUT_BIN) via $(OBJS), not invoked by name -
# if they appeared before `all`, plain `make` would build only the
# userland binary instead of the kernel.
# ---------------------------------------------------------------------
USERLAND_CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -m32 \
                  -fno-pic -fno-stack-protector -fno-omit-frame-pointer \
                  -nostdlib -nostartfiles -nodefaultlibs -I./include
USERLAND_ELF = $(USERLAND_OBJDIR)/hello.elf

$(USERLAND_OBJDIR)/hello.o: $(USERLAND_DIR)/hello.c
	@mkdir -p $(dir $@)
	$(CC) $(USERLAND_CFLAGS) -c $< -o $@

$(USERLAND_ELF): $(USERLAND_OBJDIR)/hello.o $(USERLAND_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(CC) -T $(USERLAND_DIR)/user.ld -o $@ -m32 -ffreestanding -nostdlib -static \
		-Wl,--nmagic -Wl,-s $(USERLAND_OBJDIR)/hello.o

# objcopy runs from inside $(USERLAND_OBJDIR) so the generated symbol
# names are the short, predictable _binary_hello_elf_* (objcopy derives
# them from the input path, special characters become underscores - a
# longer relative path would leak into the symbol names).
$(USERLAND_EMBED_OBJ): $(USERLAND_ELF)
	cd $(USERLAND_OBJDIR) && $(OBJCOPY) -I binary -O elf32-i386 -B i386 hello.elf hello_embed.o

# ---------------------------------------------------------------------
# AP real-mode trampoline (arch/x86/trampoline.S), embedded into the
# kernel the same way as the userland binary above: assembled and
# linked as its own tiny executable at a fixed physical load address
# (arch/x86/trampoline.ld, `. = 0x8000`) completely separate from the
# kernel image, flattened to raw bytes, then objcopy'd in as
# _binary_trampoline_bin_{start,end,size} (kernel/smp.c copies these
# bytes to physical 0x8000 before sending any AP its SIPI).
# ---------------------------------------------------------------------
TRAMPOLINE_ELF   = $(TRAMPOLINE_OBJDIR)/trampoline.elf
TRAMPOLINE_BIN   = $(TRAMPOLINE_OBJDIR)/trampoline.bin

$(TRAMPOLINE_OBJDIR)/trampoline.o: $(ARCH_DIR)/trampoline.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(TRAMPOLINE_ELF): $(TRAMPOLINE_OBJDIR)/trampoline.o $(ARCH_DIR)/trampoline.ld
	@mkdir -p $(dir $@)
	$(CC) -T $(ARCH_DIR)/trampoline.ld -o $@ -m32 -ffreestanding -nostdlib -static \
		-Wl,--nmagic -Wl,-s $(TRAMPOLINE_OBJDIR)/trampoline.o

$(TRAMPOLINE_BIN): $(TRAMPOLINE_ELF)
	$(OBJCOPY) -O binary $< $@

# objcopy runs from inside $(TRAMPOLINE_OBJDIR) for the same reason as
# the userland rule above: short, predictable _binary_trampoline_bin_*
# symbol names.
$(TRAMPOLINE_EMBED_OBJ): $(TRAMPOLINE_BIN)
	cd $(TRAMPOLINE_OBJDIR) && $(OBJCOPY) -I binary -O elf32-i386 -B i386 trampoline.bin trampoline_embed.o

# Pull in auto-generated header dependencies (silently ignored on first build
# before any .d files exist).
-include $(OBJS:.o=.d)

# Link all objects into final kernel binary
$(OUTPUT_BIN): $(OBJS) linker.ld
	@echo "Linking kernel..."
	$(CC) -T linker.ld -o $@ -m32 $(LDFLAGS) $(OBJS)

# Benchmark executable (hosted application with standard library)
BENCHMARK_DIR = benchmarks
BENCHMARK_BIN = $(BENCHMARK_DIR)/pipeline_vs_single
BENCHMARK_CFLAGS = -std=gnu99 -Wall -Wextra -O2 -I. -I./include

$(BENCHMARK_BIN): $(BENCHMARK_DIR)/pipeline_vs_single.c $(CPU_DIR)/pipeline.c $(CPU_DIR)/single_cycle.c $(CPU_DIR)/performance.c $(MEMORY_DIR)/cache.c $(MEMORY_DIR)/bus.c
	$(shell command -v gcc 2> /dev/null || echo gcc) $(BENCHMARK_CFLAGS) -o $@ \
		$(BENCHMARK_DIR)/pipeline_vs_single.c \
		$(CPU_DIR)/pipeline.c \
		$(CPU_DIR)/single_cycle.c \
		$(CPU_DIR)/performance.c \
		$(MEMORY_DIR)/cache.c \
		$(MEMORY_DIR)/bus.c

# Build and run benchmark
benchmark: $(BENCHMARK_BIN)
	@echo "Running CPU architecture benchmark..."
	@./$(BENCHMARK_BIN)

# Run kernel in QEMU (uses ISO boot)
run: $(OUTPUT_BIN)
	@echo "Running OrgitoOS in QEMU..."
	@chmod +x tools/run-qemu.sh
	@./tools/run-qemu.sh

# Create the persistent data disk image if it doesn't already exist -
# never overwritten once present, so fs/diskfs.c's on-disk data survives
# across rebuilds (only `rm disk.img` starts it fresh).
$(DISK_IMG):
	@echo "Creating $(DISK_SIZE_MB) MiB persistent disk image: $(DISK_IMG)..."
	@dd if=/dev/zero of=$(DISK_IMG) bs=1M count=$(DISK_SIZE_MB) status=none

# Create bootable ISO
iso: $(OUTPUT_BIN)
	@echo "Creating bootable ISO image..."
	@chmod +x tools/create-iso.sh
	@./tools/create-iso.sh

# Run ISO in QEMU
run-iso: iso | $(DISK_IMG)
	@echo "Running OrgitoOS ISO in QEMU..."
	@if command -v $(QEMU) &> /dev/null; then \
		$(QEMU) $(QEMU_FLAGS); \
	else \
		echo "Error: $(QEMU) not found"; \
		exit 1; \
	fi

# Debug: run in QEMU with GDB stub enabled, paused, serial output to terminal
debug: iso | $(DISK_IMG)
	@echo "Starting OrgitoOS in QEMU with GDB server enabled (paused at startup)..."
	@echo "  GDB server listening on tcp::1234"
	@echo "  Attach with: make gdb  (in another terminal)"
	@echo "  Or manually: gdb -ex \"target remote :1234\""
	@echo "  Serial output will appear below. Press Ctrl+C to quit QEMU."
	@echo ""
	@if command -v $(QEMU) &> /dev/null; then \
		$(QEMU) $(QEMU_FLAGS) -s -S -serial mon:stdio; \
	else \
		echo "Error: $(QEMU) not found"; \
		exit 1; \
	fi

# Launch GDB pre-configured to attach to a running QEMU GDB server (tcp::1234)
gdb:
	@echo "Connecting GDB to QEMU GDB server at localhost:1234..."
	@if [ -f $(OUTPUT_BIN) ]; then \
		gdb -ex "file $(OUTPUT_BIN)" -ex "target remote :1234"; \
	else \
		echo "Note: kernel binary not found; run 'make' first for symbol support."; \
		gdb -ex "target remote :1234"; \
	fi

# Run in QEMU with logging enabled (output written to qemu.log)
qemu-log: iso | $(DISK_IMG)
	@echo "Starting OrgitoOS in QEMU with logging to qemu.log..."
	@if command -v $(QEMU) &> /dev/null; then \
		$(QEMU) $(QEMU_FLAGS) -serial mon:stdio -d int,cpu_reset,guest_errors -D qemu.log; \
	else \
		echo "Error: $(QEMU) not found"; \
		exit 1; \
	fi
	@echo "QEMU log written to qemu.log"

# Run in VirtualBox
run-vbox: iso
	@echo "Running OrgitoOS in VirtualBox..."
	@chmod +x tools/run-virtualbox.sh
	@./tools/run-virtualbox.sh

# Clean build artifacts
clean:
	rm -rf $(OUTPUT_DIR)
	rm -f $(BENCHMARK_BIN)
	rm -f $(ISO_FILE)
	rm -rf iso
	@# Legacy cleanup: older Makefile versions compiled objects next to sources.
	rm -f $(ARCH_DIR)/*.o $(KERNEL_DIR)/*.o $(CPU_DIR)/*.o $(MEMORY_DIR)/*.o $(DRIVERS_DIR)/*.o $(FS_DIR)/*.o $(PROCESS_DIR)/*.o

# Show help
help:
	@echo "OrgitoOS Kernel Build System (Modular Architecture)"
	@echo "===================================================="
	@echo "Targets:"
	@echo "  all        - Build the kernel (default)"
	@echo "  benchmark  - Build and run CPU architecture benchmark"
	@echo "  clean      - Remove build artifacts"
	@echo "  run        - Build and run in QEMU (via bootable ISO)"
	@echo "  iso        - Create bootable ISO image with GRUB"
	@echo "  run-iso    - Build ISO and run in QEMU"
	@echo "  run-vbox   - Build ISO and run in VirtualBox"
	@echo "  debug      - Build ISO and run in QEMU with GDB server (paused, serial to terminal)"
	@echo "  gdb        - Launch GDB pre-configured to attach to QEMU (run 'make debug' first)"
	@echo "  qemu-log   - Build ISO and run in QEMU with logging to qemu.log"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Directory Structure:"
	@echo "  arch/x86/        - x86 architecture-specific code"
	@echo "  kernel/          - Core kernel code"
	@echo "  kernel/cpu/      - CPU simulation components (pipeline, single-cycle, performance)"
	@echo "  memory/          - Memory management (PMM, VMM, heap, cache, bus)"
	@echo "  drivers/         - Hardware drivers (console, keyboard, timer)"
	@echo "  fs/              - File systems (VFS, ramfs)"
	@echo "  process/         - Process management"
	@echo "  benchmarks/      - Benchmark programs"
	@echo "  include/         - Common headers"
	@echo "  build/obj/       - Compiled object files (mirrors source tree)"
	@echo ""
	@echo "Compiler: $(CC)"
	@echo "QEMU binary: $(QEMU)"

.PHONY: all clean run iso run-iso run-vbox debug gdb qemu-log help benchmark
