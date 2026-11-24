# MC carveout dump

Reads the 25 memory controller registers that `native_gate` in `boot/loader/loader.c` compares, from Switchroot Linux, and
prints each next to the value the OLED showed.

The values are what Linux sees. The loader reads the same registers earlier in the boot chain, and nothing guarantees the
two moments show the same carveouts. So a dump is a reference to compare against, and repeating it does not turn it into
the loader's values. `native_gate` for the Erista stays unwritten until the loader's own reading is known, which needs a
reviewed boot test of this project. The Erista row stays at `boot 0` until then.

## Run it

1. Charge the battery. Boot Switchroot Linux from hekate as usual. Nothing of this project runs.
2. Install BusyBox: `sudo apt install busybox`. The script uses `busybox devmem` and nothing else, never a standalone
   `devmem` or `devmem2` (which maps the registers writable).
3. Copy `dump.sh` to the Linux system and run it as root:

   ```sh
   sudo sh dump.sh | tee mc-erista-boot1.txt
   ```

4. Reboot into Switchroot Linux and run it again as `mc-erista-boot2.txt`, to see whether the values hold still on Linux.

Each line is the address, the value read, the OLED value and `same` or `DIFFERENT`. The script only ever calls
`busybox devmem <address> 32`, which BusyBox opens read only. Exit 2 means no BusyBox devmem, exit 3 means a register could
not be read. If the kernel refuses (`/dev/mem` restricted), stop there. Do not look for another way in.

## Running it on the OLED as well

Run it on the OLED from the same Linux. The OLED's table was read by the loader, so the comparison shows how far Linux's
view differs from the loader's on a console where both are known. If every line is `same`, Linux's view matched the
loader's there. That is evidence for the Erista, not proof.

## Why it is the way it is

`native_gate` refuses on any mismatch (0x410 plus the register's index) instead of inferring free memory, so a wrong value
either blocks the boot or lets a secure carveout be reused. The test checks the script's table against `loader.c`, so the
two cannot drift.
