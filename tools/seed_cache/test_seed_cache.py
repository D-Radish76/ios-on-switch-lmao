import hashlib
import tempfile
import unittest
import unittest.mock
from pathlib import Path

import seed_cache

MEMBERS = {'os': b'\1' * 100, 'kernelcache': b'\2' * 40}


def entry(cache: Path, data: bytes) -> Path:
    return cache / 'content_addressable' / 'sha256' / hashlib.sha256(data).hexdigest() / 'file'


class SeedCache(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        pins, blob, at = {}, bytearray(b'\0' * 7), 7
        for role, data in MEMBERS.items():
            pins[role] = {'offset': at, 'stored_size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            blob += data + b'\0' * 5
            at += len(data) + 5
        (self.dir / 'pins.bzl').write_text(f'IPSWS = {{"k": {{"members": {pins!r}}}}}\n')
        (self.dir / 'a.ipsw').write_bytes(bytes(blob))
        self.cache = self.dir / 'cache'

    def test_every_member_is_stored_by_its_sha256(self):
        with unittest.mock.patch.object(seed_cache, 'CHUNK', 16):
            seed_cache.sys.argv = [
                'x',
                'ipsw',
                '--pins',
                str(self.dir / 'pins.bzl'),
                '--kind',
                'k',
                '--ipsw',
                str(self.dir / 'a.ipsw'),
                '--cache',
                str(self.cache),
            ]
            self.assertEqual(seed_cache.main(), 0)
        for data in MEMBERS.values():
            self.assertEqual(entry(self.cache, data).read_bytes(), data)

    def test_bytes_that_differ_are_not_stored(self):
        blob = bytearray((self.dir / 'a.ipsw').read_bytes())
        blob[10] ^= 1
        (self.dir / 'a.ipsw').write_bytes(bytes(blob))
        seed_cache.sys.argv = [
            'x',
            'ipsw',
            '--pins',
            str(self.dir / 'pins.bzl'),
            '--kind',
            'k',
            '--ipsw',
            str(self.dir / 'a.ipsw'),
            '--cache',
            str(self.cache),
        ]
        self.assertEqual(seed_cache.main(), 1)
        self.assertFalse(entry(self.cache, MEMBERS['os']).exists())
        self.assertTrue(entry(self.cache, MEMBERS['kernelcache']).exists())
        self.assertEqual([p.name for p in self.cache.rglob('.part*')], [])

    def test_a_short_file_is_not_stored(self):
        data = MEMBERS['os']
        self.assertFalse(
            seed_cache.store(self.dir / 'pins.bzl', 0, len(data), hashlib.sha256(data).hexdigest(), self.cache)
        )

    def test_one_file_whole(self):
        data = MEMBERS['os']
        archive = self.dir / 'llvm.tar.xz'
        archive.write_bytes(data)
        seed_cache.sys.argv = [
            'x',
            'file',
            '--sha256',
            hashlib.sha256(data).hexdigest(),
            str(archive),
            '--cache',
            str(self.cache),
        ]
        self.assertEqual(seed_cache.main(), 0)
        self.assertEqual(entry(self.cache, data).read_bytes(), data)
        seed_cache.sys.argv = ['x', 'file', '--sha256', '0' * 64, str(archive), '--cache', str(self.cache)]
        self.assertEqual(seed_cache.main(), 1)


if __name__ == '__main__':
    unittest.main()
