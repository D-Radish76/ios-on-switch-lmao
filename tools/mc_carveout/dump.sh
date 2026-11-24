#!/bin/sh
# Reads the memory controller registers that native_gate (boot/loader/loader.c) compares, from Linux, and prints each next to
# the OLED value. Read only: the reader is only ever called as `<devmem> <address> 32`, which busybox opens O_RDONLY.
# On an Erista, run it from Switchroot Linux, twice (two boots, and two cards if there are two), and keep the output.
set -eu

if command -v devmem >/dev/null 2>&1; then
    reader="devmem"
elif command -v busybox >/dev/null 2>&1 && busybox --list | grep -qx devmem; then
    reader="busybox devmem"
else
    echo "devmem not found (apt install busybox)" >&2
    exit 2
fi

while read -r address oled; do
    [ -n "$address" ] || continue
    value=$($reader "$address" 32) || { echo "$address unreadable" >&2; exit 3; }
    if [ "$((value))" -eq "$((oled))" ]; then mark=same; else mark=DIFFERENT; fi
    printf '%s 0x%08x oled %s %s\n' "$address" "$((value))" "$oled" "$mark"
done <<'TABLE'
    0x70019050 0x00001000
    0x70019670 0x00000000
    0x70019674 0x00000000
    0x700199d4 0x00000000
    0x700199a0 0xfff00000
    0x700199a4 0x00000000
    0x700199a8 0x00000000
    0x70019648 0x00000000
    0x7001964c 0x00000000
    0x70019978 0x00000000
    0x70019c0c 0xffe00000
    0x70019c10 0x00000000
    0x70019c14 0x00000008
    0x70019c5c 0xffd00000
    0x70019c60 0x00000000
    0x70019c64 0x00000002
    0x70019cac 0xffd40000
    0x70019cb0 0x00000000
    0x70019cb4 0x00000006
    0x70019cfc 0x00000000
    0x70019d00 0x00000000
    0x70019d04 0x00000000
    0x70019d4c 0x00000000
    0x70019d50 0x00000000
    0x70019d54 0x00000000
TABLE
