// The EL2 -> EL1 sequence is the normal loader's (boot/handoff/entry.S), unchanged; this file only names its hooks.
#define probe_after_transition switch_diag_after_transition
#include "../../handoff/entry.S"
