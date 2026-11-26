"""erista_adt.py SEED_HEX OUT: the native Erista board's ADT for the full-RAM root-boot profile, one core.

Not part of any package: the Erista row is `boot 0`. //boot:erista_adt builds it so its structure can be checked.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import native_erista_adt

seed, out = sys.argv[1:]
Path(out).write_bytes(
    native_erista_adt.generate_adt(
        bytes.fromhex(seed), native_erista_adt.Profile(storage_probe=True, root_boot=True, full_ram=True)
    )
)
