// What kits/SwitchBoard asks of the kernel, on the host, over a command line, registers and device tree the test sets.
#ifndef SWITCH_TEST_SWITCH_BOARD_STUBS_H
#define SWITCH_TEST_SWITCH_BOARD_STUBS_H

#define SWITCH_BOARD_HOST_SHIM 1
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int PE_parse_boot_arg_str(const char *name, char *value, int size);

#ifdef __cplusplus
}
#endif

#endif
