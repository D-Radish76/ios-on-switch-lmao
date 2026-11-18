"""The build's compile profile held to the m2v runtime device of its shader tools, but for its bounds checks.

The pack generator's offline compile makes its translator devices of the profile
(drivers/TegraGM20BMetal/translate/compile_profile/<build>.json), which this runtime made them of before. The profile
compiles with no buffer bounds checks (policy containment UnsafeOff, isolation Off) and the frozen runtime with them, so
those two fields are the profile's own and every other one is the runtime's.
"""

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

BOUNDS_CHECKS_OFF = {'containment': 'UnsafeOff', 'isolation': 'Off'}
REFREEZE = (
    'freeze it again: raw/compile-profile <libvulkan_nouveau.dylib> <cache dir> '
    '> drivers/TegraGM20BMetal/translate/compile_profile/<build>.json, then set its policy\'s containment to UnsafeOff '
    'and isolation to Off'
)


def without_bounds_checks(profile: dict) -> dict:
    policy = {k: v for k, v in profile['policy'].items() if k not in BOUNDS_CHECKS_OFF}
    return {**profile, 'policy': policy}


def frozen() -> dict:
    return json.loads(Path(os.environ['COMPILE_PROFILE']).read_text())


class CompileProfile(unittest.TestCase):
    def test_the_profile_is_the_runtimes_but_its_bounds_checks(self):
        tools = Path(os.environ['COMPILE_PROFILE_TOOLS'])
        with tempfile.TemporaryDirectory() as scratch:
            # dyld names an input by another of its links; the runtime opens a copy of its own
            twin = Path(scratch) / 'libvulkan_nouveau.dylib'
            shutil.copyfile(tools / 'libvulkan_nouveau.dylib', twin)
            cache = Path(scratch) / 'cache'
            cache.mkdir()
            r = subprocess.run(
                [tools / 'raw' / 'compile-profile', twin, cache],
                capture_output=True,
                text=True,
                check=False,
                env={'PATH': '/usr/bin:/bin', 'HOME': scratch, 'MESA_SHADER_CACHE_DISABLE': 'true'},
            )
        self.assertEqual(r.returncode, 0, r.stderr.strip())
        self.assertEqual(without_bounds_checks(frozen()), without_bounds_checks(json.loads(r.stdout)), REFREEZE)

    def test_the_profile_compiles_with_no_bounds_checks(self):
        policy = frozen()['policy']
        self.assertEqual({k: policy[k] for k in BOUNDS_CHECKS_OFF}, BOUNDS_CHECKS_OFF)


if __name__ == '__main__':
    unittest.main()
