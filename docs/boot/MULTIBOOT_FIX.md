# GRUB Multiboot Header Fix - Technical Documentation

## Problem Statement

When booting OrgitoOS with GRUB via ISO, the following errors occurred:
```
error: no multiboot header found.
error: you need to load the kernel first.
```

## Root Cause Analysis

### The Multiboot Specification Requirement

The Multiboot v1 specification (used by GRUB) requires:
- The multiboot header MUST be within the **first 8192 bytes (8 KB)** of the kernel binary
- The header MUST be **4-byte aligned**
- The header MUST be in a **LOAD segment** (actually loaded into memory by GRUB)
- The header contains three 32-bit values: magic (0x1BADB002), flags, and checksum

### What Was Wrong

**Before the fix** (`readelf -S orgitoos.bin`):
```
  [Nr] Name              Type            Addr     Off    Size
  [ 1] .multiboot        PROGBITS        00100000 002720 00000c
  [ 2] .text             PROGBITS        00000000 000080 0018b6
```

1. `.multiboot` section was at file offset **0x2720 (10016 bytes)** — far beyond the 8KB limit
2. `.multiboot` had VMA 0x00100000, while `.text` had VMA 0x00000000
3. The linker script had `ALIGN(4K)` between sections, creating a large gap
4. `.multiboot` was **NOT included in any LOAD segment** (not loaded into memory)

### Why This Happened

The original `linker.ld`:
```ld
SECTIONS
{
  . = 1M;

  .multiboot :
  {
    *(.multiboot)
  }

  .text ALIGN(4K) :  /* <-- This ALIGN creates a gap! */
  {
    *(.text*)
  }
}
```

When the linker processes this, it places `.multiboot` at 1M (VMA 0x100000), then aligns
the next section to a 4K boundary, causing `.text` to start at a different address (0x0).
The linker creates **separate LOAD segments** for the two disjoint memory regions. Since
`.multiboot` has no following sections at 0x100000, it isn't loaded — the header exists
in the ELF file, but at the wrong offset and never loaded into memory.

## The Fix

**File:** `linker.ld`

```ld
SECTIONS
{
  . = 1M;

  /* Place multiboot header FIRST in .text section */
  .text :
  {
    *(.multiboot)    /* Multiboot header comes first */
    *(.text*)        /* Then all code */
  }

  .rodata ALIGN(4K) :
  {
    *(.rodata*)
  }

  /* ... rest of sections ... */
}
```

What changed:
1. Removed the separate `.multiboot` section — it's now part of `.text`
2. Removed the `ALIGN(4K)` between multiboot and text — they're now contiguous
3. Multiboot header is placed first via `*(.multiboot)` before `*(.text*)`

**After the fix** (`readelf -S orgitoos.bin`):
```
  [Nr] Name              Type            Addr     Off    Size
  [ 0] .text             PROGBITS        00100000 000080 0018c6
  [ 1] .rodata           PROGBITS        00102000 002080 0006a0

$ readelf -l orgitoos.bin
  LOAD           0x000080 0x00100000 0x00100000 0x026a0 0x1079b0

  Section to Segment mapping:
    00     .text .rodata .bss   <-- .text (with multiboot) is now in the LOAD segment!
```

Multiboot header now sits at offset **0x80 (128 bytes)** — well within the 8KB limit, and
included in the first LOAD segment. `grub-file --is-x86-multiboot orgitoos.bin` returns
success.

Binary layout, before vs. after (first 128 bytes):
```
BEFORE  00000080  fa bc 00 70 00 00 e8 c5  01 00 00 fa f4 eb fc 90  |...p............|
                                                                       ^ boot code, NOT multiboot

AFTER   00000080  02 b0 ad 1b 03 00 00 00  fb 4f 52 e4 fa bc 00 70  |.........OR....p|
                   ^^^^^^^^^^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^
                   Multiboot magic + flags  Checksum + boot code start
```

## Technical Details

### ELF File Layout

```
[ELF Header (52 bytes @ 0x00)]
[Program Headers (LOAD segments)]
[.text section @ 0x80]  <-- Multiboot header is here now!
  - Multiboot header (12 bytes)
  - _start code
  - Rest of code
[.rodata section]
[.data section]
[Section Headers]
```

Offset 0x80 comes from: 52-byte ELF header + 2 × 32-byte program headers = 116 bytes,
rounded up to the first LOAD segment's alignment.

### Multiboot Header Values

```c
/* In boot.S */
.set MAGIC,    0x1BADB002       /* Multiboot magic number */
.set FLAGS,    ALIGN | MEMINFO  /* 0x00000003 */
.set CHECKSUM, -(MAGIC + FLAGS) /* 0xE4524FFB */

.section .multiboot
    .align 4
    .long MAGIC      /* 0x1BADB002 */
    .long FLAGS      /* 0x00000003 */
    .long CHECKSUM   /* 0xE4524FFB */
```

GRUB scans the first 8KB for this pattern at offset 0x80:
```
Bytes:  02 b0 ad 1b  03 00 00 00  fb 4f 52 e4
        [  MAGIC  ]  [  FLAGS  ]  [CHECKSUM ]
```

## Verification

The automated tool (`./tools/verify-multiboot.sh`) runs five checks — GRUB detection,
ELF section layout, LOAD segment inclusion, header content (magic/flags/checksum), and a
raw hexdump — and prints a pass/fail summary. To check manually:

```bash
# 1. GRUB's own detector
grub-file --is-x86-multiboot build/orgitoos.bin   # exit code 0 = success

# 2. Section layout
objdump -h build/orgitoos.bin | grep .text        # .text at file offset 0x80

# 3. LOAD segments
readelf -l build/orgitoos.bin | grep LOAD         # LOAD starts at offset 0x80

# 4. Header bytes
hexdump -C build/orgitoos.bin -s 0x80 -n 12       # 02 b0 ad 1b 03 00 00 00 fb 4f 52 e4

# 5. Full boot test
make iso && qemu-system-i386 -cdrom orgitoos.iso -boot d
```

## Key Takeaways for OSDev Engineers

**Don't:**
```ld
.multiboot : { *(.multiboot) }
.text ALIGN(4K) : { *(.text*) }  /* ALIGN creates a gap! */
```

**Do:**
```ld
.text : {
    *(.multiboot)  /* First! */
    *(.text*)      /* Then code */
}
```

1. Never use `ALIGN()` between the multiboot and text sections — it breaks GRUB detection.
2. Always place the multiboot header in the first LOAD segment — GRUB only scans loaded memory.
3. Keep the multiboot header within the first 8KB — non-negotiable per the Multiboot spec.
4. Verify with `grub-file --is-x86-multiboot` and `readelf -l` after every linker script change.

## References

- [GNU GRUB Multiboot Specification v1](https://www.gnu.org/software/grub/manual/multiboot/multiboot.html)
- [OSDev Wiki - Bare Bones](https://wiki.osdev.org/Bare_Bones)
- [OSDev Wiki - Multiboot](https://wiki.osdev.org/Multiboot)
- [OSDev Wiki - Linker Scripts](https://wiki.osdev.org/Linker_Scripts)
