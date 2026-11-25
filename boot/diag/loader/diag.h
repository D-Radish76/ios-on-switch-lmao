#ifndef SWITCH_DIAG_H
#define SWITCH_DIAG_H

typedef void (*switch_diag_say_fn)(const char *text, int bad);

/* Waits SWITCH_DIAG_WAIT_SECONDS on the CPU's counter, then PSCI SYSTEM_RESET. When the counter cannot be trusted, or
 * the reset returns, says so and parks: nothing here ever continues to a kernel. */
void switch_diag_exit(switch_diag_say_fn say) __attribute__((noreturn));
#endif
