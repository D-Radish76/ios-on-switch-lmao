import unittest

import control_cpu_test as switch_control


class SwitchNativeTimerCPU(switch_control.ControlCPU):
    switch_sources = ('timer.c', 'timer_native_test.c', 'timer_native_test.S', 'control_test_start.S')
    switch_cases = 4
    switch_link_flags = ('-Wl,--section-start=.switch_native_timeout=0x5444c',)


if __name__ == '__main__':
    unittest.main()
