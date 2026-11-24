# MC carveout dump

Reads the 25 memory controller registers that `native_gate` in `boot/loader/loader.c` compares, from Switchroot Linux, and
prints each next to the value the OLED showed. It is how the Erista's carveout values get measured without booting this
project: the Erista row stays at `boot 0` until a dump exists.

## Run it

1. Charge the battery. Boot Switchroot Linux from hekate as usual. Nothing of this project runs.
2. Get a `devmem` that only reads: `sudo apt install busybox`. Do not use `devmem2`, which maps the registers writable.
3. Copy `dump.sh` to the Linux system and run it as root:

   ```sh
   sudo sh dump.sh | tee mc-erista-boot1.txt
   ```

4. Reboot into Switchroot Linux and run it again as `mc-erista-boot2.txt`. Repeat on a second card if there is one.

Each line is the address, the value read, the OLED value and `same` or `DIFFERENT`. The script only ever calls
`devmem <address> 32`, which busybox opens read only. Exit 2 means no `devmem`, exit 3 means a register could not be read.
If the kernel refuses (`/dev/mem` restricted), stop there. Do not look for another way in.

## What to send back

The two or more output files. All lines `same` means the OLED's table already fits. Any `DIFFERENT` line is the Erista's
value for that register, and only values equal across every dump go into `native_gate`.

## Why it is the way it is

`native_gate` refuses on any mismatch (0x410 plus the register's index) instead of inferring free memory, so a wrong value
here either blocks the boot or, worse, lets a secure carveout be reused. The values come from hardware or not at all.
The test checks the script's table against `loader.c`, so the two cannot drift.
