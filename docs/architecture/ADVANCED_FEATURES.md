# Advanced Features: IPC, SMP, GUI, Networking, Scripting

Five subsystems beyond the Phase 0 core (boot, memory, drivers, VFS) and Phase 1
process management: inter-process communication, multi-core detection, a simulated
GUI/windowing layer, a basic networking stack, and shell scripting.

## 1. IPC Mechanisms (Inter-Process Communication)

**Location:** `include/ipc.h`, `kernel/ipc.c`

### Pipes
- Circular buffer-based implementation, 4KB buffer per pipe, up to 32 pipes
- Reader/writer PIDs tracked; non-blocking read/write

```c
pipe_t* pipe_create(uint32_t reader_pid, uint32_t writer_pid);
int pipe_write(pipe_t* pipe, const void* data, size_t size);
int pipe_read(pipe_t* pipe, void* buffer, size_t size);
void pipe_close(pipe_t* pipe);
```

### Message queues
- Fixed-size queue (16 messages, 256 bytes max per message), up to 32 queues
- FIFO delivery, type-based filtering

```c
msg_queue_t* msgqueue_create(uint32_t owner_pid);
int msgqueue_send(msg_queue_t* queue, uint32_t sender_pid, uint32_t type, const void* data, size_t size);
int msgqueue_receive(msg_queue_t* queue, message_t* msg);
void msgqueue_close(msg_queue_t* queue);
```

**Test:** `test_ipc` | **Future work:** semaphores/mutexes, shared memory, signals.

## 2. Multi-core SMP Detection

**Location:** `include/smp.h`, `kernel/smp.c`

CPUID-based detection and per-CPU bookkeeping for up to 16 CPUs (ONLINE/OFFLINE/
HALTED state tracking, Bootstrap Processor identification). **Application Processor
boot-up is not implemented** — see the `TODO: Implement AP boot sequence with APIC`
markers in `kernel/smp.c` — so this detects multiple cores but the kernel still runs
single-core in practice.

```c
void smp_init(void);
uint32_t smp_get_cpu_count(void);
uint32_t smp_get_current_cpu(void);
cpu_info_t* smp_get_cpu_info(uint32_t cpu_id);
int smp_boot_ap(uint32_t cpu_id);   /* stub */
```

**Test:** `test_smp` | **Future work:** real APIC/LAPIC-driven AP bring-up, per-CPU
kernel stacks, CPU affinity, spinlocks.

## 3. GUI / Windowing System (simulated framebuffer)

**Location:** `include/gui.h`, `kernel/gui.c`

An 800×600×32 **in-memory** framebuffer (not real VGA/VESA hardware output) with
window creation, show/hide, and drawing primitives (pixels, rects, lines, text).

```c
void gui_init(void);
window_t* gui_create_window(int x, int y, int width, int height, const char* title);
void gui_show_window(window_t* window);
void gui_hide_window(window_t* window);
void gui_render_window(window_t* window);
void gui_draw_pixel(int x, int y, uint32_t color);
void gui_draw_rect(rect_t* rect, uint32_t color);
void gui_draw_filled_rect(rect_t* rect, uint32_t color);
```

**Test:** `test_gui` | **Future work:** real VBE/VESA mode switching to an actual
display, bitmap font rendering, mouse support, widgets.

## 4. Networking Stack (structures only)

**Location:** `include/network.h`, `kernel/network.c`

Ethernet frame, IP header, and socket-interface structures for ICMP/TCP/UDP, with
checksum calculation. **Packet transmission, reception, and socket send/receive are
stubs** — see the `TODO: Implement actual packet transmission/reception` markers in
`kernel/network.c` — there is no real NIC driver or protocol state machine behind
the API yet.

```c
void net_init(void);
socket_t* net_socket_create(uint8_t protocol);
int net_socket_bind(socket_t* socket, uint16_t port);
int net_socket_connect(socket_t* socket, ip_addr_t* ip, uint16_t port);
int net_socket_send(socket_t* socket, const void* data, size_t size);   /* stub */
int net_socket_recv(socket_t* socket, void* buffer, size_t size);       /* stub */
```

**Test:** `test_net` | **Future work:** a real driver (e1000/RTL8139), ARP, DHCP,
a working TCP state machine, packet queueing.

## 5. Shell Scripting

**Location:** `include/script.h`, `kernel/script.c`

Variable storage (64 variables) with substitution, `if`/`while`/`for` control flow,
and script execution from strings. **Pipe-based I/O redirection between commands is
a stub** — see the `TODO: Execute cmd1, redirect output to pipe...` marker in
`kernel/script.c`.

```c
void script_init(void);
int script_execute(const char* script);
int script_set_var(const char* name, const char* value);
const char* script_get_var(const char* name);
int script_parse_if(const char* condition);
int script_create_pipe(const char* cmd1, const char* cmd2);   /* stub */
```

Default variables: `PATH=/bin:/usr/bin`, `HOME=/home`, `PS1=OrgitoOS>`.

**Test:** `test_script` | **Future work:** an expression parser, function
definitions, background jobs, command history, a working pipe implementation.

## Boot sequence

These five subsystems initialize after the Phase 0 core and VFS, before process
management:

```
1. Console          5. Timer (100 Hz)     9. SMP support
2. IDT               6. Keyboard           10. GUI framework
3. Exceptions        7. VFS                11. Networking stack
4. PIC                8. IPC mechanisms    12. Shell scripting
```

## Directory layout

```
include/{ipc,smp,gui,network,script}.h   # Public headers
kernel/{ipc,smp,gui,network,script}.c    # Implementations
```

## Testing

```bash
make clean && make && make run
```
```
OrgitoOS> test_ipc      # pipes and message queues
OrgitoOS> test_smp      # CPU detection and status
OrgitoOS> test_gui      # window creation and rendering
OrgitoOS> test_net      # network device and socket structures
OrgitoOS> test_script   # variables and script execution
```
