import unittest

import control_cpu_test as switch_control


class SchedulerCPU(switch_control.ControlCPU):
    switch_sources = ('diagnostic.c', 'scheduler.c', 'timer.c', 'scheduler_test.c', 'control_test_start.S')
    switch_cases = 36


if __name__ == '__main__':
    unittest.main()
