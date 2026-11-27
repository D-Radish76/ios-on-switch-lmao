"""ERISTA_FIRST_XNU_REMAINING.md: the holds it keeps and the documents it points at."""

import re
import unittest
from pathlib import Path

NAME = 'ERISTA_FIRST_XNU_REMAINING.md'
TEXT = Path(NAME).read_text()


class Remaining(unittest.TestCase):
    def test_the_four_groups_and_the_holds(self) -> None:
        for heading in ('## A. ', '## B. ', '## C. ', '## D. ', '## Dependencies', '## Shortest order'):
            self.assertIn(heading, TEXT)
        for phrase in ('`boot` stays 0', 'T3 and T4 are not waived', 'first XNU run is on HOLD', 'Nothing here lowers a HOLD'):
            self.assertIn(phrase, TEXT)

    def test_style(self) -> None:
        self.assertNotRegex(TEXT, '[\\u2013\\u2014]')
        self.assertNotRegex(TEXT, '(?i)ioswitch')
        self.assertLess(len(TEXT.splitlines()), 500)

    def test_every_named_document_exists(self) -> None:
        for name in set(re.findall(r'`(ERISTA_[A-Z0-9_]+\.md)`', TEXT)):
            self.assertTrue(Path(name).is_file(), name)


if __name__ == '__main__':
    unittest.main()
