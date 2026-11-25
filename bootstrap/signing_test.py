import plistlib
import subprocess
import tempfile
import unittest
from pathlib import Path

from bootstrap.signing import CodesignError, run_checked, sign_bundle


class SigningTests(unittest.TestCase):
    def test_bundle_seals_resources_info_and_nested_code(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            source = work / 'source.app'
            nested = source / 'PlugIns' / 'nested.appex'
            nested.mkdir(parents=True)
            fixture = work / 'main'
            subprocess.run(
                ['/usr/bin/xcrun', '--sdk', 'macosx', 'clang', '-arch', 'arm64', '-x', 'c', '-', '-o', str(fixture)],
                input=b'int main(void) { return 0; }\n',
                capture_output=True,
                check=True,
            )
            for bundle, identifier in (
                (source, 'com.brunostjohn.switch.fixture'),
                (nested, 'com.brunostjohn.switch.plugin'),
            ):
                (bundle / 'Info.plist').write_bytes(
                    plistlib.dumps(
                        {
                            'CFBundleIdentifier': identifier,
                            'CFBundleExecutable': 'main',
                            'CFBundlePackageType': 'APPL' if bundle == source else 'XPC!',
                        }
                    )
                )
                (bundle / 'main').write_bytes(fixture.read_bytes())
                (bundle / 'main').chmod(0o755)
            (source / 'resource.txt').write_text('sealed resource')
            entitlements = work / 'entitlements.plist'
            entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))
            signed = sign_bundle(source, work / 'signed.app', entitlements)
            self.assertTrue((signed / '_CodeSignature/CodeResources').is_file())
            self.assertTrue((signed / 'PlugIns/nested.appex/_CodeSignature/CodeResources').is_file())
            for relative in ('Info.plist', 'resource.txt', 'PlugIns/nested.appex/main'):
                with self.subTest(relative=relative):
                    target = signed / relative
                    original = target.read_bytes()
                    target.write_bytes(original + b'changed')
                    result = subprocess.run(
                        ['/usr/bin/codesign', '--verify', '--deep', '--strict', str(signed)],
                        capture_output=True,
                        check=False,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    target.write_bytes(original)

    def test_a_failed_command_reports_its_output(self):
        with self.assertRaises(CodesignError) as raised:
            run_checked(['/bin/sh', '-c', 'echo seen-out; echo seen-err >&2; exit 3'])
        text = str(raised.exception)
        for expected in ('exit status 3', 'seen-out', 'seen-err'):
            self.assertIn(expected, text)

    def test_a_command_that_works_says_nothing(self):
        run_checked(['/usr/bin/true'])


if __name__ == '__main__':
    unittest.main()
