# Detyra 1 – Kopjimi i një skedari me `open()`, `read()`, `write()`, `close()` në OrgitoOS

Kjo dosje përmban zgjidhjen e **Detyrës 1** (Sisteme Operative). Në vend të një makine virtuale Ubuntu,
detyra u realizua në **OrgitoOS**, sistemin operativ të këtij repozitori, i ekzekutuar në makinë virtuale
**QEMU** (`make run`). Për krahasim, i njëjti program u ekzekutua edhe në Linux me `strace` të vërtetë.

Raporti i plotë (në shqip, me screenshot-e): **[`Detyre1_Zgjidhja.docx`](Detyre1_Zgjidhja.docx)**

## Skedarët e dorëzimit

| Skedari | Përshkrimi |
|---|---|
| [`userland/FileCopy.c`](../../../userland/FileCopy.c) | Kodi burim i programit (kompilohet si në OrgitoOS ashtu edhe në Linux) |
| [`gjurma.txt`](gjurma.txt) | Gjurma e një kopjimi të suksesshëm, e prodhuar nga `strace` i OrgitoOS |
| [`gjurma_linux.txt`](gjurma_linux.txt) | E njëjta gjurmë e prodhuar nga `strace` 6.8 në Linux (krahasim) |
| [`Detyre1_Zgjidhja.docx`](Detyre1_Zgjidhja.docx) | Raporti: versioni i kernelit, kompilimi, 5 testet, përgjigjet e pyetjeve |
| [`screenshots/`](screenshots) | Pamjet nga ekrani i OrgitoOS (QEMU) dhe terminali i Linux-it |

## Çfarë u shtua në OrgitoOS

| Pjesa | Skedari | Përshkrimi |
|---|---|---|
| Thirrjet e sistemit për skedarë | `kernel/fileio.c`, `kernel/fileio.h` | `open`, `read`, `write`, `close` mbi VFS; tabelë përshkruesish për çdo proces (0/1/2 = stdin/stdout/stderr, skedari i parë merr 3); `errno` me vlerat e Linux-it; `O_CREAT\|O_EXCL` atomik; shkrime të pjesshme kur skedari arrin kufirin 4096 B |
| Numrat e syscall-eve | `kernel/syscall.h`, `kernel/syscall.c` | `SYS_OPEN`=9, `SYS_CLOSE`=10, `SYS_FDREAD`=11, `SYS_FDWRITE`=12, të gjitha përmes `int 0x80` |
| Libraria e përdoruesit | `include/ulibc.h`, `kernel/ulibc.c` | `open/read/write/close` → `int $0x80`; konvertim `-errno` → `-1` + `errno`; `strerror()` |
| Programi | `userland/FileCopy.c` | Kompilohet me `-std=c99 -Wall -Wextra -D__ORGITOOS__` (Makefile) |
| `strace` | `kernel/fileio.c`, `kernel/posix_commands.c` | Gjurmon `openat/read/write/close` në formatin e `strace`; `-o skedar`, `-e trace=...`, `-V` |
| Komanda të reja | `kernel/posix_commands.c` | `FileCopy` (ose `./FileCopy`), `strace`, `chmod`, `cmp`, `whoami`, `layout full\|split` |
| Shell-i | `kernel/shell.c` | Status daljeje për çdo komandë dhe `cmd1 && cmd2` |
| Të tjera | `kernel/commands.c`, `drivers/console.c` | `uname -r`, `stat` tregon lejet (`Access: (0644/-rw-r--r--)`), konsolë 80 kolona |

## Si ta ekzekutoni

```bash
make run
```

Në shell-in e OrgitoOS:

```text
layout full
whoami
pwd
uname -r
strace -V
write burimi.txt Une jam Emri Mbiemri
./FileCopy                      (burimi.txt, kopja.txt)
cmp burimi.txt kopja.txt && echo Skedaret jane identike
strace -o gjurma.txt -e trace=open,openat,read,write,close ./FileCopy   (burimi.txt, kopja_trace.txt)
cat gjurma.txt
```

## Gjurma (OrgitoOS)

```text
write(1, "Emri i skedarit burim: ", 23) = 23
read(0, "burimi.txt\n", 512) = 11
write(1, "Emri i skedarit destinacion: ", 29) = 29
read(0, "kopja_trace.txt\n", 512) = 16
openat(AT_FDCWD, "burimi.txt", O_RDONLY) = 3
openat(AT_FDCWD, "kopja_trace.txt", O_WRONLY|O_CREAT|O_EXCL, 0644) = 4
read(3, "Une jam Orgito Leka\n", 4096) = 20
write(4, "Une jam Orgito Leka\n", 20) = 20
read(3, "", 4096) = 0
close(3) = 0
close(4) = 0
write(1, "Kopjimi perfundoi me sukses: 20 "..., 77) = 77
+++ exited with 0 +++
```

1. **Përshkruesit:** burimi mori **3**, destinacioni mori **4**.
2. **Bajtet:** `read()` kërkoi **4096** bajte dhe mori **20**; programi i kaloi `write()` pikërisht **20** bajte, dhe `write()` ktheu 20.
3. **Fundi i skedarit:** `read(3, "", 4096) = 0`, `read()` ktheu **0**.

## Kufizime

- Programet ring 3 në OrgitoOS kanë ende një bug të njohur (Invalid Opcode, shih README-në kryesore).
  Prandaj `FileCopy` ekzekutohet brenda procesit të shell-it, por **çdo** operacion me skedarë kalon
  realisht përmes portës `int 0x80` dhe `syscall_dispatch()`, pikërisht si te një program përdoruesi.
- Sistemi i skedarëve (VFS) është në RAM: skedarët humbasin pas rindezjes dhe kanë maksimumi 4096 bajte.
- OrgitoOS ka një përdorues të vetëm (`orgito`) dhe nuk ka kompilues C brenda tij: programi kompilohet në
  host me `gcc` dhe lidhet me kernelin.
