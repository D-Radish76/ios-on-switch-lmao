import unittest

import control_cpu_test as switch_control


class CandidateControlCPU(switch_control.ControlCPU):
    switch_sources = ('diagnostic.c', 'candidate_control.c', 'scheduler.c', 'candidate_control_test.c', 'control_test_start.S')
    switch_cases = 38


class SwitchCandidateControlLTOCPU(CandidateControlCPU):
    switch_link_flags = ('-flto',)


if __name__ == '__main__':
    unittest.main()
