// PE_parse_boot_arg_str for kits/SwitchBoard on the host, over a command line the test sets.
#ifndef SWITCH_TEST_SWITCH_BOARD_STUBS_H
#define SWITCH_TEST_SWITCH_BOARD_STUBS_H

#ifdef __cplusplus
extern "C" {
#endif

int PE_parse_boot_arg_str(const char *name, char *value, int size);

#ifdef __cplusplus
}
#endif

#endif
