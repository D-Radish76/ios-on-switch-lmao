#ifndef SWITCH_VM_RESET_H
#define SWITCH_VM_RESET_H
/* The boot-arg and the tick arithmetic of the timed reset after VM initialisation (SwitchVmDone.c). Pure functions, so the host
 * tests run exactly what the kernel runs. */
#include <stdint.h>

#define SWITCH_VM_RESET_ARG             "switch_stop_after_vm"
#define SWITCH_VM_RESET_DEFAULT_SECONDS 15u
#define SWITCH_VM_RESET_MAX_SECONDS     60u
#define SWITCH_PSCI_SYSTEM_RESET        0x84000009u
#define SWITCH_VM_RESET_LINE_BYTES      1024u

enum switch_vm_reset_arg {
    SWITCH_VM_RESET_ABSENT,
    SWITCH_VM_RESET_VALID,
    SWITCH_VM_RESET_INVALID,
};

static inline int switch_vm_reset_is_space(char c)
{
    return c == ' ' || c == '\t';
}

/* Absent: no word of that name. Valid: exactly one, bare (the default) or `=<1..60>` in plain decimal. Anything else, a
 * repeated word included, is invalid and the caller must not arm a reset. */
static inline enum switch_vm_reset_arg switch_vm_reset_parse(const char *line, unsigned *seconds)
{
    static const char name[] = SWITCH_VM_RESET_ARG;
    const unsigned name_length = (unsigned)sizeof(name) - 1u;
    unsigned found = 0;
    unsigned value = SWITCH_VM_RESET_DEFAULT_SECONDS;
    unsigned bad = 0;
    unsigned i = 0;
    if (!line)
        return SWITCH_VM_RESET_ABSENT;
    while (i < SWITCH_VM_RESET_LINE_BYTES && line[i]) {
        while (i < SWITCH_VM_RESET_LINE_BYTES && switch_vm_reset_is_space(line[i]))
            ++i;
        const unsigned start = i;
        while (i < SWITCH_VM_RESET_LINE_BYTES && line[i] && !switch_vm_reset_is_space(line[i]))
            ++i;
        const unsigned length = i - start;
        if (length < name_length)
            continue;
        unsigned same = 1;
        for (unsigned k = 0; k < name_length; ++k)
            same &= line[start + k] == name[k];
        if (!same)
            continue;
        if (length == name_length) {
            ++found;
            value = SWITCH_VM_RESET_DEFAULT_SECONDS;
        } else if (line[start + name_length] == '=') {
            ++found;
            unsigned digits = 0;
            value = 0;
            for (unsigned k = start + name_length + 1u; k < start + length; ++k, ++digits) {
                if (line[k] < '0' || line[k] > '9' || value > SWITCH_VM_RESET_MAX_SECONDS) {
                    bad = 1;
                    break;
                }
                value = value * 10u + (unsigned)(line[k] - '0');
            }
            if (!digits || value < 1u || value > SWITCH_VM_RESET_MAX_SECONDS)
                bad = 1;
        }
    }
    if (!found)
        return SWITCH_VM_RESET_ABSENT;
    if (found > 1u || bad)
        return SWITCH_VM_RESET_INVALID;
    *seconds = value;
    return SWITCH_VM_RESET_VALID;
}

/* Counter ticks for the wait; 0 when the frequency is 0, the seconds are out of range or the product would overflow. */
static inline uint64_t switch_vm_reset_ticks(uint64_t frequency, unsigned seconds)
{
    if (!frequency || seconds < 1u || seconds > SWITCH_VM_RESET_MAX_SECONDS || frequency > UINT64_MAX / seconds)
        return 0;
    return frequency * seconds;
}

/* True while the counter has not reached the deadline, across a wrap of the 64-bit count. */
static inline int switch_vm_reset_before(uint64_t now, uint64_t deadline)
{
    return (int64_t)(now - deadline) < 0;
}
#endif
