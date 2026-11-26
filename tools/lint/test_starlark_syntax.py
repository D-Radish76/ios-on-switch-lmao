import unittest

import starlark_syntax


class StarlarkSyntaxTest(unittest.TestCase):
    def test_chained_comparison_is_rejected(self):
        self.assertTrue(starlark_syntax.check_source("x = 1 <= y <= 60\n"))

    def test_split_comparison_is_accepted(self):
        self.assertEqual(starlark_syntax.check_source("x = y < 1 or y > 60\n"), [])

    def test_python_only_statements_are_rejected(self):
        for source in ("while True:\n    pass\n", "import os\n", "class A:\n    pass\n", "x = {1, 2}\n", "x = f'{a}'\n", "if a:\n    b = 1\n"):
            self.assertTrue(starlark_syntax.check_source(source), source)


if __name__ == "__main__":
    unittest.main()
