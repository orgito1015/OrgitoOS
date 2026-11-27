/*
 * OrgitoOS - Kernel Main Entry Point
 * 
 * This is the main entry point for the OrgitoOS kernel after boot.
 * It initializes all kernel subsystems and enters the main kernel loop.
 */

#include "kernel.h"
#include "shell.h"
#include "syscall.h"
#include "../arch/x86/gdt.h"
#include "../arch/x86/idt.h"
#include "../arch/x86/pic.h"
#include "../arch/x86/isr.h"
#include "../arch/x86/exceptions.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../drivers/console.h"
#include "../drivers/serial.h"
#include "../fs/vfs.h"
#include "../fs/diskfs.h"
#include "../drivers/ata.h"
#include "exec.h"
#include "../include/ipc.h"
#include "../include/smp.h"
#include "../include/gui.h"
#include "../include/network.h"
#include "../include/script.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../memory/heap.h"
#include "../process/process.h"
#include "../process/scheduler.h"

/* Current working directory */
vfs_node_t* current_directory = 0;

/* Get current directory */
vfs_node_t* kernel_get_current_directory(void) {
    return current_directory;
}

/* Set current directory */
void kernel_set_current_directory(vfs_node_t* dir) {
    if (dir && dir->type == NODE_DIRECTORY) {
        current_directory = dir;
    }
}

/*
 * Boot banner, printed once to screen before any process exists (so it
 * always lands on the left pane - see drivers/console.c's current_pane()
 * default). Every line is kept to at most 39 columns: a line that hits
 * exactly the pane's 40-column width trips pane_put_char()'s own
 * end-of-line auto-wrap *and* the explicit '\n' right after it, which
 * would double up as a stray blank row.
 */
static void print_boot_banner(void) {
    console_write("\n");
    console_write("=======================================\n");
    console_write("                OrgitoOS\n");
    console_write("      Advanced Educational Kernel\n");
    console_write("                 v0.1.0\n");
    console_write("=======================================\n");
    console_write("\n");
}

/* Kernel entry point called from boot.S */
void kmain(struct multiboot_info *mboot) {
    /* Initialize console */
    console_init();

    print_boot_banner();

    /*
     * Subsystem-by-subsystem init trace: kept off the (narrow, 40-column)
     * screen so boot stays clean, but still written straight to the
     * serial port so it's not lost - `make debug`/`make qemu-log` and
     * `qemu-system-i386 ... -serial stdio` (see drivers/serial.h) still
     * show the full step-by-step log for debugging.
     */
    serial_write("OrgitoOS - Advanced Educational Kernel\n");
    serial_write("Running in 32-bit protected mode.\n\n");

    /* Install the kernel's own GDT + TSS (required for ring 3) */
    serial_write("[1/15] Installing GDT and TSS...\n");
    gdt_init();

    /* Initialize IDT */
    serial_write("[2/15] Initializing IDT...\n");
    idt_init();

    /* Install exception handlers */
    serial_write("[3/15] Installing exception handlers...\n");
    exceptions_init();

    /* Initialize PIC */
    serial_write("[4/15] Initializing PIC...\n");
    pic_init();

    /* Initialize timer (100 Hz) */
    serial_write("[5/15] Initializing timer...\n");
    timer_init(100);

    /* Install timer interrupt handler (IRQ0 = interrupt 0x20) */
    idt_set_gate(0x20, (uint32_t)irq0_handler, KERNEL_CODE_SEGMENT, IDT_FLAGS_KERNEL);

    /* Install keyboard interrupt handler (IRQ1 = interrupt 0x21) */
    serial_write("[6/15] Initializing keyboard...\n");
    idt_set_gate(0x21, (uint32_t)irq1_handler, KERNEL_CODE_SEGMENT, IDT_FLAGS_KERNEL);

    /* Initialize keyboard (this will unmask IRQ1) */
    keyboard_init();

    /* Initialize physical memory manager */
    serial_write("[8/15] Initializing physical memory manager...\n");
    pmm_init(mboot);

    /* Initialize virtual memory manager */
    serial_write("[9/15] Initializing virtual memory manager...\n");
    vmm_init();

    /*
     * Initialize the kernel heap on a PMM-backed region.
     *
     * A contiguous run of physical pages inside the kernel's identity
     * map (virt == phys), so the heap base is valid as a pointer.
     * 2048 pages = 8 MiB of dynamic kernel memory (file data, ELF images
     * being loaded, kernel stacks).
     */
    {
        #define HEAP_PAGES 2048u     /* 8 MiB */
        void *heap_start = pmm_alloc_pages(HEAP_PAGES);
        if (heap_start != 0) {
            heap_init(heap_start, (size_t)HEAP_PAGES * 0x1000u);
        }
        #undef HEAP_PAGES
    }

    /* Initialize VFS (after the heap: file data is heap-allocated) */
    serial_write("[7/15] Initializing filesystem...\n");
    vfs_init();
    current_directory = vfs_get_root();

    /* Programs in /bin: ELF files built from userland/ and embedded in
     * the kernel image (kernel/exec.c). */
    exec_install_embedded_programs();

    /* Persistent disk: ATA driver + OrgitoFS mounted at /mnt. Without a
     * disk (QEMU started without disk.img) /mnt is simply absent. */
    serial_write("[7b/15] Detecting disk and mounting /mnt...\n");
    if (ata_init() == 0) {
        diskfs_init();
    }

    /* Initialize IPC mechanisms */
    serial_write("[10/15] Initializing IPC (pipes, message queues)...\n");
    ipc_init();

    /* Initialize SMP support */
    serial_write("[11/15] Initializing multi-core SMP...\n");
    smp_init();

    /* Initialize GUI/Windowing system */
    serial_write("[12/15] Initializing GUI framework...\n");
    gui_init();

    /* Initialize Networking stack */
    serial_write("[13/15] Initializing networking stack...\n");
    net_init();

    /* Initialize Shell scripting */
    serial_write("[14/15] Initializing shell scripting...\n");
    script_init();

    /* Initialize process management and system calls */
    serial_write("[15/15] Initializing processes, scheduler, syscalls...\n");
    process_init();
    syscall_init();

    /* Enable interrupts */
    __asm__ __volatile__("sti");

    /* Now that interrupts are enabled, unmask the timer IRQ */
    pic_unmask_irq(0);

    /* Full readiness detail goes to serial only (see the trace above);
     * the screen just gets a short, calm ready line. */
    serial_write("\n*** System Ready ***\n");
    serial_write("- Exception handling: Active\n");
    serial_write("- Timer interrupts: 100 Hz (preemptive, 50 ms quantum)\n");
    serial_write("- Keyboard: Ready\n");
    serial_write("- Filesystem: Ready\n");
    serial_write("- Processes: Multilevel round-robin scheduler\n");
    serial_write("- Syscalls: int 0x80 (exit/write/getpid/fork/...)\n");
    serial_write("- User mode: Ring 3 execution via TSS\n");
    serial_write("- IPC: Pipes and message queues available\n");
    serial_write("- Network: TCP/IP stack initialized\n\n");

    console_write("System ready. Type 'help' for commands.\n\n");

    /*
     * Register shell commands once, before any shell process exists: two
     * shell instances (one per split-screen pane, below) share a single
     * command table, so this must not run twice - see shell_init()'s own
     * idempotency guard for why.
     */
    shell_init();

    /*
     * Launch the interactive shell as a proper high-priority process,
     * then start preemptive scheduling. kmain's own context (adopted
     * as PID 0 by process_init) becomes the idle process: it runs only
     * when nothing else is READY and simply halts until the next
     * interrupt.
     *
     * Two instances are launched, one per split-screen pane (drivers/
     * console.h): each shell_task() claims its pane from the `arg` it's
     * passed. Tab switches which pane has keyboard focus.
     */
    process_create("shell-left", shell_task, (void*)CONSOLE_PANE_LEFT, PRIORITY_HIGH);
    process_create("shell-right", shell_task, (void*)CONSOLE_PANE_RIGHT, PRIORITY_HIGH);
    scheduler_start();

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

/* Interactive shell, running as its own process (see kmain). One instance
 * runs per split-screen pane; `arg` (CONSOLE_PANE_LEFT/CONSOLE_PANE_RIGHT)
 * says which. */
void shell_task(void *arg) {
    process_current()->pane = (int)(intptr_t)arg;

    char input[256];
    while (1) {
        console_write("OrgitoOS> ");
        keyboard_get_line(input, sizeof(input));
        shell_execute(input);
    }
}
