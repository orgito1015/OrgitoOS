# GRUB Boot Order & Device Detection Fix

This documents two related QEMU/GRUB boot issues and their fixes, applied in
sequence (the second fix supersedes part of the first).

## Issue 1: QEMU tries floppy/hard-disk before CD-ROM

**Problem:** QEMU's default boot order tries floppy (`a`) and hard disk (`c`)
before CD-ROM (`d`), causing failed boot attempts and a delayed boot process.

**Fix:** Pass `-boot d` to `qemu-system-i386` to force CD-ROM as the first boot
device. This is set in `tools/run-qemu.sh` and `Makefile`'s `QEMU_FLAGS`.

## Issue 2: Invalid GRUB2 root device syntax

**Problem:** `grub.cfg` briefly included `set root='(cd0)'` to explicitly pin the
boot device. This turned out to be **invalid GRUB2 device syntax** — `(cd0)` isn't
a real GRUB2 device identifier — which caused:
```
error: no server is specified.
error: you need to load the kernel first.
```

**Root cause:** GRUB2 doesn't use `(cd0)`; when it can't resolve an invalid root
device, it fails to find the kernel at `/boot/orgitoos.bin` even though the file
is present on the ISO.

**Fix:** Remove the `set root=...` line entirely and let GRUB auto-detect the
root device. When `grub-mkrescue` builds the ISO, GRUB is embedded with its own
device-detection logic and automatically sets `$root` to the ISO filesystem it
booted from — no manual root device specification is needed.

**Current `grub.cfg`:**
```grub
set timeout=3
set default=0

menuentry "OrgitoOS" {
    multiboot /boot/orgitoos.bin
    boot
}
```

### GRUB2 device naming reference
- `(hd0)` — first hard disk
- `(hd0,1)` — first partition on first hard disk
- `(cd)` — CD-ROM device (when booting from ISO)
- `(cd0)` — **invalid** in GRUB2 (this was the bug)

## Related fix: unused-parameter compiler warning

Separately, `kernel_panic_ext()` in `kernel/panic.c` took a `line` parameter that
was never used (line-number formatting in panic messages needs `sprintf()`, not
yet implemented in the kernel), producing:
```
kernel/panic.c:26:66: warning: unused parameter 'line' [-Wunused-parameter]
```
Fixed with an explicit `(void)line;` cast and explanatory comment — see
`kernel/panic.c`.

## Verification

```bash
make clean && make          # clean compile, no unused-parameter warning
make iso                    # builds orgitoos.iso
make run                    # boots via QEMU with -boot d
```

Expected: GRUB menu shows "OrgitoOS", boots after the 3-second timeout with no
`no server is specified` / `you need to load the kernel first` errors, and the
kernel initializes normally.

## Related documentation

- [Multiboot Header Fix](MULTIBOOT_FIX.md) — separate fix for multiboot header
  placement within the ELF binary (unrelated to GRUB device resolution).
