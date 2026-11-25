import plistlib
import subprocess
import tempfile
import unittest
from pathlib import Path

from bootstrap.signing import CodesignError, materialize_links, run_checked, sign_bundle


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

    def test_links_out_of_the_bundle_become_files_and_inner_links_stay(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            outside = root / 'outside'
            outside.mkdir()
            (outside / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': 'App'}))
            (outside / 'App').write_bytes(b'binary')
            (outside / 'extra.txt').write_text('extra')
            (outside / 'dir').mkdir()
            (outside / 'dir' / 'f').write_text('f')
            bundle = root / 'App.app'
            (bundle / 'Frameworks' / 'F.framework' / 'Versions' / 'A').mkdir(parents=True)
            (bundle / 'Info.plist').symlink_to(outside / 'Info.plist')
            (bundle / 'App').symlink_to(outside / 'App')
            (bundle / 'extra.txt').symlink_to(outside / 'extra.txt')
            (bundle / 'dir').symlink_to(outside / 'dir')
            versions = bundle / 'Frameworks' / 'F.framework' / 'Versions'
            (versions / 'Current').symlink_to('A')
            materialize_links(bundle)
            for name in ('Info.plist', 'App', 'extra.txt'):
                self.assertFalse((bundle / name).is_symlink(), name)
                self.assertTrue((bundle / name).is_file(), name)
            self.assertEqual((bundle / 'App').read_bytes(), b'binary')
            self.assertTrue((bundle / 'dir').is_dir() and not (bundle / 'dir').is_symlink())
            self.assertEqual((bundle / 'dir' / 'f').read_text(), 'f')
            self.assertTrue((versions / 'Current').is_symlink())
            self.assertEqual((outside / 'App').read_bytes(), b'binary')
            self.assertTrue((outside / 'App').is_file() and not (outside / 'App').is_symlink())

    def test_an_inner_link_that_is_the_main_executable_becomes_a_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary) / 'App.app'
            (bundle / 'bin').mkdir(parents=True)
            (bundle / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': 'App'}))
            (bundle / 'bin' / 'real').write_bytes(b'x')
            (bundle / 'App').symlink_to('bin/real')
            materialize_links(bundle)
            self.assertFalse((bundle / 'App').is_symlink())

    def test_a_link_to_nothing_stops_the_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary) / 'App.app'
            bundle.mkdir()
            (bundle / 'gone').symlink_to(Path(temporary) / 'missing')
            with self.assertRaises(CodesignError):
                materialize_links(bundle)


if __name__ == '__main__':
    unittest.main()
