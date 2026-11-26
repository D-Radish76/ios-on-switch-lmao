"""The action-graph check: the seed is followed through inputs and outputs, and every action it reaches must be uncacheable."""

import re
import subprocess
import unittest

import seeded_taint as taint

TAGS = '{no-cache: 1, no-remote: 1}'


def action(mnemonic, inputs, outputs, info=TAGS):
    return {'mnemonic': mnemonic, 'inputs': inputs, 'outputs': outputs, 'info': info}


def compress(count=128, info=TAGS):
    # They read a directory the layout step wrote, which the input listing does not name: the walk cannot see them.
    return [action('KernelCompress', [f'out/blocks/{i:03}'], [f'out/lz4/{i:03}'], info) for i in range(count)]


def graph():
    return [
        action('Gen', ['local/erista-seed.hex', 'tool'], ['out/adt']),
        action('BootAdt', ['out/adt', 'apple.adt'], ['out/ios.adt']),
        action('KernelLayout', ['out/ios.adt', 'kernel'], ['out/plan', 'out/shard0']),
        action('KernelWindow', ['out/plan', 'out/shard0'], ['out/window']),
        action('BootScript', ['out/window'], ['out/script']),
        action('BootBl33', ['out/script', 'out/window'], ['out/bl33']),
        action('BootPackage', ['out/bl33'], ['out/pkg']),
        action('Unrelated', ['kernel', 'tool'], ['out/other'], info=''),
    ] + compress()


def render(actions):
    return ''.join(
        f"action 'x'\n  Mnemonic: {a['mnemonic']}\n  Inputs: [{', '.join(a['inputs'])}]\n"
        f'  Outputs: [{", ".join(a["outputs"])}]\n' + (f'  ExecutionInfo: {a["info"]}\n' if a['info'] else '')
        for a in actions
    )


def fake(actions, returncode=0):
    seen = []

    def run(command, **_):
        seen.append(command)
        by_name = re.search(r'mnemonic\("(.*)", deps', command[-1])
        if by_name:
            hits = [a for a in actions if a['mnemonic'] == by_name.group(1)]
        else:
            wanted = re.search(r'inputs\("(.*)", deps', command[-1]).group(1)
            hits = [a for a in actions if any(re.fullmatch(wanted, i) for i in a['inputs'])]
        return subprocess.CompletedProcess(command, returncode, render(hits), '')

    run.seen = seen
    return run


class Follows(unittest.TestCase):
    def test_the_seed_is_followed_down_every_dependent_and_not_beyond(self):
        found = taint.tainted((), fake(graph()))
        self.assertEqual(
            {a.mnemonic for a in found},
            {'Gen', 'BootAdt', 'KernelLayout', 'KernelWindow', 'BootScript', 'BootBl33', 'BootPackage'},
        )

    def test_an_action_that_only_a_later_round_reaches_is_found(self):
        counts = taint.seed_actions_are_private((), fake(graph()))
        self.assertEqual(counts['BootPackage'], 1)
        self.assertEqual(counts['KernelCompress'], 128)
        self.assertNotIn('Unrelated', counts)

    def test_the_query_uses_the_same_flags_the_build_does_and_ends_with_the_flag_off(self):
        run = fake(graph())
        taint.seed_actions_are_private(['--jobs=4'], run)
        first = run.seen[0]
        self.assertEqual(first[:2], ['bazel', 'aquery'])
        self.assertLess(first.index('--jobs=4'), first.index('--remote_cache='))
        self.assertIn('--//boot:erista_boot_run=false', first)
        self.assertIn('--bes_backend=', first)

    def test_a_path_is_matched_by_what_it_names_and_not_only_by_a_seeded_looking_name(self):
        chain = ('BootAdt', 'KernelLayout', 'KernelWindow', 'BootScript', 'BootBl33', 'BootPackage')
        renamed = [action('Gen', ['local/erista-seed.hex'], ['out/plain.bin'])]
        previous = 'out/plain.bin'
        for n, mnemonic in enumerate(chain):
            renamed.append(action(mnemonic, [previous], [f'out/x{n}']))
            previous = f'out/x{n}'
        self.assertEqual(len(taint.tainted((), fake(renamed))), 7)


class Refuses(unittest.TestCase):
    def test_an_action_without_either_tag_is_refused(self):
        for info in ('', '{no-cache: 1}', '{no-remote: 1}', '{requires-darwin: 1}'):
            actions = graph()
            actions[3] = action('KernelWindow', ['out/plan', 'out/shard0'], ['out/window'], info=info)
            with self.subTest(info=info), self.assertRaisesRegex(taint.TaintError, 'KernelWindow'):
                taint.seed_actions_are_private((), fake(actions))

    def test_a_tag_that_only_looks_like_one_does_not_count(self):
        actions = graph()
        actions[1] = action('BootAdt', ['out/adt'], ['out/ios.adt'], info='{no-cache-please: 1, my-no-remote: 1}')
        with self.assertRaises(taint.TaintError):
            taint.seed_actions_are_private((), fake(actions))

    def test_nothing_reads_the_seed_or_a_required_action_is_missing(self):
        with self.assertRaisesRegex(taint.TaintError, 'no action reads the seed'):
            taint.seed_actions_are_private((), fake([a for a in graph() if a['mnemonic'] != 'Gen']))
        with self.assertRaisesRegex(taint.TaintError, 'BootPackage'):
            taint.seed_actions_are_private((), fake([a for a in graph() if a['mnemonic'] != 'BootPackage']))

    def test_a_failed_or_unreadable_query_refuses(self):
        with self.assertRaises(taint.TaintError):
            taint.seed_actions_are_private((), fake(graph(), returncode=1))
        with self.assertRaises(taint.TaintError):
            taint.parse("action 'x'\n  Inputs: [a]\n")

    def test_an_internal_action_needs_no_tag_but_an_unknown_one_does(self):
        actions = graph() + [action('FileWrite', ['out/pkg'], ['out/list'], info='')]
        self.assertEqual(taint.seed_actions_are_private((), fake(actions))['FileWrite'], 1)
        actions = graph() + [action('Mystery', ['out/pkg'], ['out/list'], info='')]
        with self.assertRaisesRegex(taint.TaintError, 'Mystery'):
            taint.seed_actions_are_private((), fake(actions))

    def test_a_graph_that_never_stops_growing_is_refused(self):
        def run(command, **_):
            n = len(run.rounds)
            run.rounds.append(n)
            return subprocess.CompletedProcess(command, 0, render([action('Loop', ['x'], [f'out/{n}'])]), '')

        run.rounds = []
        with self.assertRaisesRegex(taint.TaintError, 'still spreading'):
            taint.tainted((), run)


class FixedSet(unittest.TestCase):
    def without_compress(self):
        return [a for a in graph() if a['mnemonic'] != 'KernelCompress']

    def test_the_walk_alone_would_have_passed_without_them_and_the_fixed_set_does_not(self):
        with self.assertRaisesRegex(taint.TaintError, '0 KernelCompress actions found, 128 expected'):
            taint.seed_actions_are_private((), fake(self.without_compress()))

    def test_too_few_or_too_many_are_refused(self):
        for count in (0, 1, 127, 129, 256):
            with (
                self.subTest(count=count),
                self.assertRaisesRegex(taint.TaintError, f'{count} KernelCompress actions found'),
            ):
                taint.seed_actions_are_private((), fake(self.without_compress() + compress(count)))

    def test_a_duplicate_is_refused_even_when_the_count_is_right(self):
        twice = self.without_compress() + compress(127) + [compress(1)[0]]
        with self.assertRaisesRegex(taint.TaintError, 'listed twice'):
            taint.seed_actions_are_private((), fake(twice))

    def test_one_action_without_a_tag_is_refused(self):
        for info in ('', '{no-cache: 1}', '{no-remote: 1}'):
            actions = (
                self.without_compress()
                + compress(127)
                + [action('KernelCompress', ['out/blocks/x'], ['out/lz4/x'], info)]
            )
            with (
                self.subTest(info=info),
                self.assertRaisesRegex(taint.TaintError, 'KernelCompress holds the seed and lacks'),
            ):
                taint.seed_actions_are_private((), fake(actions))

    def test_an_action_the_walk_also_finds_is_counted_once(self):
        both = [a for a in graph() if a['outputs'] != ['out/lz4/000']]
        both.append(action('KernelCompress', ['out/shard0'], ['out/lz4/000']))
        found = {a.mnemonic for a in taint.tainted((), fake(both))}
        self.assertIn('KernelCompress', found)
        self.assertEqual(taint.seed_actions_are_private((), fake(both))['KernelCompress'], 128)

    def test_the_other_mnemonic_in_the_query_is_not_counted(self):
        noisy = graph() + [action('KernelCompressOther', ['x'], ['out/other-lz4'], info='')]
        self.assertEqual(taint.seed_actions_are_private((), fake(noisy))['KernelCompress'], 128)

    def test_the_module_documents_the_tree_artifact_blind_spot(self):
        self.assertIn('tree artifact', taint.__doc__)
        self.assertEqual(taint.FIXED_SETS, {'KernelCompress': 128})


class Helpers(unittest.TestCase):
    def test_the_pattern_only_ever_widens(self):
        pattern = taint.pattern({'out/a+b', 'local/erista-seed.hex'})
        self.assertTrue(re.fullmatch(pattern, 'out/a+b'))
        self.assertTrue(re.fullmatch(pattern, 'local/erista-seed.hex'))

    def test_the_required_actions_are_the_ones_the_mac_listed_and_more(self):
        for name in ('BootBl33', 'BootPackage', 'BootScript'):
            self.assertIn(name, taint.REQUIRED)


if __name__ == '__main__':
    unittest.main()
