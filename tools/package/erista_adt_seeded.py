"""erista_adt_seeded.py SEED_FILE OUT: erista_adt.py with the per-install seed (erista_seed.py) in place of the pinned one."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import erista_seed
import native_erista_adt

seed_file, out = sys.argv[1:]
try:
    seed = erista_seed.load(Path(seed_file))
except erista_seed.SeedError as exc:
    raise SystemExit(f'refused: {exc}')
Path(out).write_bytes(
    native_erista_adt.generate_adt(seed, native_erista_adt.Profile(storage_probe=True, root_boot=True, full_ram=True))
)
