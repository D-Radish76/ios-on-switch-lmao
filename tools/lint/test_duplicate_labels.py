import unittest

import duplicate_labels


class DuplicateLabelsTest(unittest.TestCase):
    def test_a_repeated_data_label_is_found(self):
        text = 'py_test(name = "t", data = ["//:A.md", "//:B.md", "//:A.md"])\n'
        self.assertEqual(["pkg: t (data): //:A.md is listed more than once"], duplicate_labels.find(text, "pkg"))

    def test_the_same_label_written_two_ways_is_one_label(self):
        text = 'filegroup(name = "g", srcs = [":a.c", "//pkg:a.c", "a.c"])\n'
        self.assertEqual(["pkg: g (srcs): //pkg:a.c is listed more than once"], duplicate_labels.find(text, "pkg"))

    def test_distinct_labels_and_repeated_flags_are_fine(self):
        text = 'cc_library(name = "l", srcs = [":a.c", ":b.c"], copts = ["-I", "x", "-I", "y"], deps = ["//a:x", "//b:x"])\n'
        self.assertEqual([], duplicate_labels.find(text, "pkg"))

    def test_the_reported_attribute_and_rule_are_named(self):
        text = 'py_test(name = "diag_t4r_test", deps = ["//tools:a", "//tools:a"], data = ["//:x"])\n'
        self.assertEqual(["p: diag_t4r_test (deps): //tools:a is listed more than once"], duplicate_labels.find(text, "p"))

    def test_the_stand_in_refuses_a_duplicate_too(self):
        import runfiles_check

        collector = runfiles_check.Collector()
        rule = {"kind": "py_test", "name": "x", "data": ["//:A.md", "//:A.md"], "srcs": [], "deps": []}
        collector.rule("tools/package/test", rule)
        self.assertTrue([p for p in collector.problems if "listed more than once" in p])


if __name__ == "__main__":
    unittest.main()
