#ifndef T4A0_SEQUENCE_H
#define T4A0_SEQUENCE_H

#include "gate.h"

/* The arming order as the model runs it: idle check, then each write drawn, written, read back. Never parks; the caller asks may_park. */
int t4a_model_run(t4a_gate *gate, const t4a_io *io);

#endif
