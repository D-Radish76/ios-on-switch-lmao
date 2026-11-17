import unittest

import control_cpu_test as switch_control


class OwnedTimerCPU(switch_control.ControlCPU):
    switch_sources = ('timer.c', 'timer_owned_test.c', 'control_test_start.S')
    switch_cases = 13


if __name__ == '__main__':
    unittest.main()
