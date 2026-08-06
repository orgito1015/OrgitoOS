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
QEMU_FLAGS = -cdrom $(ISO_FILE) -boot d

# Compiler selection: prefer cross-compiler, fallback to native gcc
CC := $(shell command -v i686-elf-gcc 2> /dev/null || echo gcc)

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
C_SOURCES = $(ARCH_DIR)/gdt.c $(ARCH_DIR)/idt.c $(ARCH_DIR)/pic.c $(ARCH_DIR)/exceptions.c \
            $(KERNEL_DIR)/kernel.c $(KERNEL_DIR)/syscall.c $(KERNEL_DIR)/user_programs.c \
            $(KERNEL_DIR)/proc_commands.c $(KERNEL_DIR)/panic.c $(KERNEL_DIR)/string.c \
            $(KERNEL_DIR)/shell.c $(KERNEL_DIR)/commands.c $(KERNEL_DIR)/ipc.c \
            $(KERNEL_DIR)/smp.c $(KERNEL_DIR)/gui.c $(KERNEL_DIR)/network.c $(KERNEL_DIR)/script.c \
            $(CPU_DIR)/pipeline.c $(CPU_DIR)/single_cycle.c $(CPU_DIR)/performance.c \
            $(MEMORY_DIR)/pmm.c $(MEMORY_DIR)/vmm.c $(MEMORY_DIR)/heap.c \
            $(MEMORY_DIR)/cache.c $(MEMORY_DIR)/bus.c \
            $(DRIVERS_DIR)/console.c $(DRIVERS_DIR)/serial.c $(DRIVERS_DIR)/keyboard.c $(DRIVERS_DIR)/timer.c \
            $(FS_DIR)/vfs.c \
            $(PROCESS_DIR)/process.c $(PROCESS_DIR)/scheduler.c

# Assembly sources (excluding exceptions.S, handled separately above)
ASM_SOURCES = $(ARCH_DIR)/boot.S $(ARCH_DIR)/isr.S $(ARCH_DIR)/context.S $(ARCH_DIR)/syscall.S

# Object files: mirror the source tree under build/obj/
C_OBJS   = $(patsubst %.c,$(OBJDIR)/%.o,$(C_SOURCES))
ASM_OBJS = $(patsubst %.S,$(OBJDIR)/%.o,$(ASM_SOURCES))
EXCEPTIONS_ASM_OBJ = $(OBJDIR)/$(ARCH_DIR)/exceptions_asm.o

OBJS = $(C_OBJS) $(ASM_OBJS) $(EXCEPTIONS_ASM_OBJ)

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

# Create bootable ISO
iso: $(OUTPUT_BIN)
	@echo "Creating bootable ISO image..."
	@chmod +x tools/create-iso.sh
	@./tools/create-iso.sh

# Run ISO in QEMU
run-iso: iso
	@echo "Running OrgitoOS ISO in QEMU..."
	@if command -v $(QEMU) &> /dev/null; then \
		$(QEMU) $(QEMU_FLAGS); \
	else \
		echo "Error: $(QEMU) not found"; \
		exit 1; \
	fi

# Debug: run in QEMU with GDB stub enabled, paused, serial output to terminal
debug: iso
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
qemu-log: iso
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
