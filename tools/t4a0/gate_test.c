#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gate.h"
#include "sequence.h"
#include "sim.h"

static unsigned checks, failures;

#define CHECK(condition, ...) \
    do { \
        checks++; \
        if (!(condition)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

typedef struct rig {
    t4a_sim sim;
    t4a_io io;
    t4a_gate gate;
    t4a_params params;
    int once;
} rig;

static void rig_init(rig *r, uint32_t source)
{
    CHECK(t4a_params_make(&r->params, source, 4, 250000) == 0, "params for source %u", source);
    t4a_sim_init(&r->sim, r->params.ptv_address);
    r->io = t4a_sim_io(&r->sim);
    r->once = 0;
    t4a_gate_init(&r->gate, &r->params, &r->once);
}

static void plan(const rig *r, uint32_t address[6], uint32_t value[6])
{
    const uint32_t a[6] = {r->params.ptv_address, T4A_CONFIG, T4A_COMMAND, T4A_UNLOCK, T4A_COMMAND, r->params.ptv_address};
    const uint32_t v[6] = {r->params.ptv_word, r->params.config_word, T4A_START, T4A_UNLOCK_KEY, T4A_DISABLE, 0};
    memcpy(address, a, sizeof a);
    memcpy(value, v, sizeof v);
}

static int allowed_write(const rig *r, const t4a_access *w)
{
    uint32_t address[6], value[6];
    plan(r, address, value);
    for (int i = 0; i < 6; i++)
        if (w->address == address[i] && w->value == value[i] && w->width == 32)
            return 1;
    return 0;
}

static unsigned write_count(const t4a_sim *sim)
{
    unsigned n = 0;
    for (unsigned i = 0; i < sim->logged; i++)
        n += sim->log[i].is_write;
    return n;
}

/* Brings a fresh rig to the named state with its previous step read back and drawn. */
static void reach(rig *r, enum t4a_state state)
{
    uint32_t address[6], value[6];
    plan(r, address, value);
    if (state == T4A_INIT)
        return;
    CHECK(t4a_gate_checked(&r->gate, &r->io) == T4A_ALLOWED, "idle check");
    t4a_gate_draw(&r->gate);
    for (int i = 0; i < 3 && state != T4A_CHECKED && r->gate.state != state; i++) {
        CHECK(t4a_gate_write(&r->gate, &r->io, address[i], 32, value[i]) == T4A_ALLOWED, "write %d", i + 1);
        CHECK(t4a_gate_verify(&r->gate, &r->io) == T4A_ALLOWED, "verify %d", i + 1);
        t4a_gate_draw(&r->gate);
    }
    CHECK(r->gate.state == state, "reached state %d, wanted %d", r->gate.state, state);
}

static void nominal(void)
{
    static const uint32_t sources[] = {5, 0, 7};
    for (unsigned s = 0; s < 3; s++) {
        rig r;
        uint32_t address[6], value[6];
        rig_init(&r, sources[s]);
        plan(&r, address, value);
        CHECK(t4a_model_run(&r.gate, &r.io) == T4A_ALLOWED, "nominal run, source %u", sources[s]);
        CHECK(r.gate.state == T4A_ARMED && t4a_gate_may_park(&r.gate), "armed and verified");
        CHECK(write_count(&r.sim) == 3 && r.sim.stray_writes == 0 && r.sim.starts == 1, "three writes, one start");
        for (unsigned i = 0, w = 0; i < r.sim.logged; i++)
            if (r.sim.log[i].is_write) {
                CHECK(r.sim.log[i].address == address[w] && r.sim.log[i].value == value[w] && r.sim.log[i].width == 32,
                      "write %u order", w + 1);
                w++;
            }
        CHECK(t4a_sim_counting(&r.sim), "the model counts after START");
    }
}

static void parameters(void)
{
    t4a_params p;
    CHECK(t4a_params_make(&p, 5, 0, 1000) != 0, "period 0 refused");
    CHECK(t4a_params_make(&p, 5, 256, 1000) != 0, "period 256 refused");
    CHECK(t4a_params_make(&p, 5, 1, 0) != 0, "count 0 refused");
    CHECK(t4a_params_make(&p, 5, 1, 0x20000000u) != 0, "count above 29 bits refused");
    CHECK(t4a_params_make(&p, 1, 1, 1000) != 0 && t4a_params_make(&p, 8, 1, 1000) != 0, "unknown sources refused");
    CHECK(t4a_params_make(&p, 5, 255, 0x1fffffffu) == 0, "upper bounds accepted");
    CHECK(p.config_word == (5u | (255u << 4) | (1u << 15)) && p.ptv_word == (0xc0000000u | 0x1fffffffu), "words");
    CHECK(t4a_params_make(&p, 5, 1, 1) == 0 && p.ptv_address == 0x60005060u, "TMR5 is a candidate with PTV 0x60005060");
}

static void request(rig *r, enum t4a_state state, uint32_t address, unsigned width, uint64_t value, int is_expected,
                    const char *what)
{
    unsigned before = write_count(&r->sim);
    int verdict = t4a_gate_write(&r->gate, &r->io, address, width, value);
    unsigned made = write_count(&r->sim) - before;

    if (is_expected) {
        CHECK(verdict == T4A_ALLOWED && made == 1, "%s: the expected write passes in state %d", what, state);
        return;
    }
    switch (state) {
    case T4A_INIT:
    case T4A_CHECKED:
        CHECK(verdict == T4A_REFUSED_RUN && r->gate.state == T4A_REFUSED && made == 0, "%s: refused with no write (state %d)", what, state);
        break;
    case T4A_PTV_SET:
    case T4A_CONFIG_SET:
        CHECK(verdict == T4A_REFUSED_RUN && r->gate.state == T4A_STOPPED && made == 3 && !t4a_sim_counting(&r->sim),
              "%s: refused and stopped with three stop writes (state %d)", what, state);
        break;
    default:
        CHECK(verdict == T4A_DENIED && r->gate.state == T4A_ARMED && made == 0 && r->sim.starts == 1,
              "%s: denied while armed and verified (state %d)", what, state);
    }
    for (unsigned i = 0; i < r->sim.logged; i++)
        if (r->sim.log[i].is_write)
            CHECK(allowed_write(r, &r->sim.log[i]), "%s: only planned writes reach the block", what);
}

static void matrix(void)
{
    static const enum t4a_state states[] = {T4A_INIT, T4A_CHECKED, T4A_PTV_SET, T4A_CONFIG_SET, T4A_ARMED};
    static const unsigned widths[] = {8, 16, 32, 64};
    static const uint32_t foreign[] = {0x600050c0u, 0x6000511cu, 0x70000000u + 0xe400u, 0x60006028u, 0x60005064u, 0x60005104u,
                                       0x60005010u, 0u, 0xffffffffu, 0x60005101u};
    for (unsigned s = 0; s < 5; s++) {
        enum t4a_state state = states[s];
        uint32_t address[6], value[6];
        rig probe;
        rig_init(&probe, 5);
        plan(&probe, address, value);
        int next = state == T4A_CHECKED ? 0 : state == T4A_PTV_SET ? 1 : state == T4A_CONFIG_SET ? 2 : -1;

        for (int w = 0; w < 6; w++) {
            rig r;
            rig_init(&r, 5);
            reach(&r, state);
            request(&r, state, address[w], 32, value[w], w == next, "planned write");
        }
        for (unsigned f = 0; f < sizeof foreign / sizeof foreign[0]; f++)
            for (int w = 0; w < 3; w++) {
                rig r;
                rig_init(&r, 5);
                reach(&r, state);
                request(&r, state, foreign[f], 32, value[w], 0, "foreign address");
            }
        for (unsigned wd = 0; wd < 4; wd++)
            if (widths[wd] != 32)
                for (int w = 0; w < 3; w++) {
                    rig r;
                    rig_init(&r, 5);
                    reach(&r, state);
                    request(&r, state, address[w], widths[wd], value[w], 0, "bad width");
                }
        for (int w = 0; w < 3; w++) {
            for (int bit = 0; bit < 64; bit++) {
                rig r;
                rig_init(&r, 5);
                reach(&r, state);
                request(&r, state, address[w], 32, (uint64_t)value[w] ^ (1ull << bit), 0, "single bit flip");
            }
            for (int d = -4; d <= 4; d += 4) {
                rig r;
                rig_init(&r, 5);
                reach(&r, state);
                if (d)
                    request(&r, state, address[w] + d, 32, value[w], 0, "neighbour address");
            }
            rig r;
            rig_init(&r, 5);
            reach(&r, state);
            request(&r, state, address[w], 32, 0, w == 5, "zero value");
        }
    }
}

static void unsettled(void)
{
    uint32_t address[6], value[6];
    for (int skip = 0; skip < 2; skip++)
        for (enum t4a_state state = T4A_CHECKED; state <= T4A_CONFIG_SET; state++) {
            rig r;
            rig_init(&r, 5);
            plan(&r, address, value);
            reach(&r, state);
            if (skip == 0)
                r.gate.line_drawn = 0;
            else if (state == T4A_CHECKED)
                continue;
            else
                r.gate.read_back_ok = 0;
            int idx = state - T4A_CHECKED;
            CHECK(t4a_gate_write(&r.gate, &r.io, address[idx], 32, value[idx]) == T4A_REFUSED_RUN,
                  "state %d: the right write is refused until the line is drawn and read back", state);
        }
}

static void terminal(void)
{
    uint32_t address[6], value[6];
    static const enum t4a_state wanted[] = {T4A_REFUSED, T4A_STOPPED, T4A_STOP_FAILED, T4A_HUNG};
    for (unsigned t = 0; t < 4; t++) {
        rig r;
        rig_init(&r, 5);
        plan(&r, address, value);
        if (wanted[t] == T4A_REFUSED)
            t4a_gate_write(&r.gate, &r.io, 0x1234, 32, 0);
        else if (wanted[t] == T4A_STOPPED) {
            reach(&r, T4A_PTV_SET);
            t4a_gate_write(&r.gate, &r.io, 0x1234, 32, 0);
        } else if (wanted[t] == T4A_STOP_FAILED) {
            reach(&r, T4A_PTV_SET);
            t4a_sim_inject(&r.sim, r.sim.accesses + 1, T4A_FAULT_BUS);
            t4a_gate_write(&r.gate, &r.io, 0x1234, 32, 0);
        } else {
            reach(&r, T4A_PTV_SET);
            t4a_sim_inject(&r.sim, r.sim.accesses, T4A_FAULT_HANG);
            t4a_gate_write(&r.gate, &r.io, address[1], 32, value[1]);
        }
        CHECK(r.gate.state == wanted[t], "terminal state %d reached, got %d", wanted[t], r.gate.state);
        unsigned writes = write_count(&r.sim), accesses = r.sim.accesses;
        for (int w = 0; w < 6; w++)
            CHECK(t4a_gate_write(&r.gate, &r.io, address[w], 32, value[w]) == T4A_DENIED, "terminal %d: write %d denied", wanted[t], w);
        CHECK(write_count(&r.sim) == writes && r.gate.state == wanted[t], "terminal %d: nothing written, state kept", wanted[t]);
        CHECK(t4a_gate_verify(&r.gate, &r.io) == T4A_DENIED, "terminal %d: no verify", wanted[t]);
        if (wanted[t] == T4A_STOP_FAILED || wanted[t] == T4A_HUNG) {
            uint64_t v;
            CHECK(t4a_gate_read(&r.gate, &r.io, T4A_CONFIG, 32, &v) == T4A_DENIED && r.sim.accesses == accesses,
                  "terminal %d: no access of any kind", wanted[t]);
        }
    }
}

static void start_only_once(void)
{
    rig r;
    uint32_t address[6], value[6];
    rig_init(&r, 5);
    plan(&r, address, value);
    CHECK(t4a_model_run(&r.gate, &r.io) == T4A_ALLOWED, "first arm");
    for (int i = 0; i < 3; i++)
        CHECK(t4a_gate_write(&r.gate, &r.io, T4A_COMMAND, 32, T4A_START) == T4A_DENIED, "second START denied while armed");
    CHECK(r.sim.starts == 1 && r.gate.start_writes == 1, "one START");

    t4a_gate_init(&r.gate, &r.params, &r.once);
    unsigned writes = write_count(&r.sim);
    CHECK(t4a_model_run(&r.gate, &r.io) == T4A_DENIED && r.gate.state == T4A_REFUSED && r.gate.cause == T4A_CAUSE_SECOND_ARM,
          "a new gate in the same run starts refused");
    CHECK(write_count(&r.sim) == writes && r.sim.starts == 1, "no write from the refused gate");

    rig f;
    rig_init(&f, 5);
    reach(&f, T4A_CONFIG_SET);
    t4a_sim_inject(&f.sim, f.sim.accesses, T4A_FAULT_BUS);
    t4a_gate_write(&f.gate, &f.io, T4A_COMMAND, 32, T4A_START);
    CHECK(f.once == 1, "a START that faulted still counts as the one arm");
    t4a_gate_init(&f.gate, &f.params, &f.once);
    CHECK(t4a_gate_write(&f.gate, &f.io, T4A_COMMAND, 32, T4A_START) == T4A_DENIED, "no START after a faulted START");
}

static int log_is_arm_then_stop(const rig *r)
{
    uint32_t address[6], value[6];
    unsigned w = 0, s = 3;
    plan(r, address, value);
    for (unsigned i = 0; i < r->sim.logged; i++) {
        const t4a_access *a = &r->sim.log[i];
        if (!a->is_write)
            continue;
        if (!allowed_write(r, a))
            return 0;
        if (s == 3 && w < 3 && a->address == address[w] && a->value == value[w])
            w++;
        else if (w >= 1 && s < 6 && a->address == address[s] && a->value == value[s])
            s++;
        else
            return 0;
    }
    return 1;
}

static void fault_injection(void)
{
    rig nominal_run;
    rig_init(&nominal_run, 5);
    t4a_model_run(&nominal_run.gate, &nominal_run.io);
    unsigned n = nominal_run.sim.accesses, seen[10] = {0};

    for (unsigned index = 0; index < n + 2; index++)
        for (int kind = T4A_FAULT_BUS; kind < T4A_FAULT_KINDS; kind++) {
            rig r;
            rig_init(&r, 5);
            t4a_sim_inject(&r.sim, index, kind);
            t4a_model_run(&r.gate, &r.io);
            const t4a_gate *g = &r.gate;
            unsigned hangs = 0, last_write = 0;
            for (unsigned i = 0; i < r.sim.logged; i++) {
                hangs += r.sim.log[i].status == T4A_HANG;
                if (r.sim.log[i].is_write)
                    last_write = i + 1;
            }
            seen[g->state]++;
            CHECK(g->state == T4A_ARMED || g->state == T4A_STOPPED || g->state == T4A_STOP_FAILED ||
                  g->state == T4A_REFUSED || g->state == T4A_HUNG, "access %u kind %d: ends in a settled state, got %d", index, kind, g->state);
            CHECK(r.sim.starts <= 1 && g->start_writes <= 1 && (kind == T4A_FAULT_CORRUPT || r.sim.stray_writes == 0),
                  "access %u kind %d: at most one START, no stray write unless the model itself corrupts one", index, kind);
            CHECK(log_is_arm_then_stop(&r), "access %u kind %d: writes are the arm order then the stop order", index, kind);
            CHECK(g->writes == write_count(&r.sim), "access %u kind %d: the gate counted every write", index, kind);
            CHECK(hangs <= 1 && (g->state == T4A_HUNG) == (hangs == 1), "access %u kind %d: a hang ends all access", index, kind);
            if (g->state == T4A_HUNG)
                CHECK(r.sim.logged == r.sim.accesses && r.sim.log[r.sim.logged - 1].status == T4A_HANG, "access %u kind %d: the hang is the last access", index, kind);
            if (g->state == T4A_REFUSED)
                CHECK(write_count(&r.sim) == 0, "access %u kind %d: a refusal before the first write writes nothing", index, kind);
            if (g->state == T4A_STOP_FAILED)
                CHECK(g->stop_step <= 3 && last_write >= 1, "access %u kind %d: stop failure keeps its step", index, kind);
            if (g->state == T4A_STOPPED)
                CHECK(!t4a_sim_counting(&r.sim) && g->cause != T4A_CAUSE_NONE, "access %u kind %d: stopped means not counting in the model", index, kind);
            if (g->state == T4A_ARMED)
                CHECK(t4a_gate_may_park(g) && write_count(&r.sim) == 3, "access %u kind %d: armed only after three writes read back", index, kind);
        }
    CHECK(seen[T4A_ARMED] && seen[T4A_STOPPED] && seen[T4A_REFUSED] && seen[T4A_HUNG], "single faults reach armed, stopped, refused and hung");
    printf("fault injection: %u accesses x 4 kinds: armed %u, stopped %u, stop failed %u, refused %u, hung %u\n", n, seen[T4A_ARMED],
           seen[T4A_STOPPED], seen[T4A_STOP_FAILED], seen[T4A_REFUSED], seen[T4A_HUNG]);
}

static void fault_pairs(void)
{
    rig nominal_run;
    rig_init(&nominal_run, 5);
    t4a_model_run(&nominal_run.gate, &nominal_run.io);
    unsigned n = nominal_run.sim.accesses + 6, seen[10] = {0};

    for (unsigned i = 0; i < n; i++)
        for (unsigned j = i + 1; j < n; j++)
            for (int a = T4A_FAULT_BUS; a < T4A_FAULT_KINDS; a++)
                for (int b = T4A_FAULT_BUS; b < T4A_FAULT_KINDS; b++) {
                    rig r;
                    rig_init(&r, 5);
                    t4a_sim_inject(&r.sim, i, a);
                    t4a_sim_inject(&r.sim, j, b);
                    t4a_model_run(&r.gate, &r.io);
                    seen[r.gate.state]++;
                    CHECK(r.sim.starts <= 1 && r.gate.start_writes <= 1, "pair %u,%u: one START at most", i, j);
                    CHECK(a == T4A_FAULT_CORRUPT || b == T4A_FAULT_CORRUPT || log_is_arm_then_stop(&r), "pair %u,%u kinds %d,%d: write order", i, j, a, b);
                    CHECK(r.gate.writes == write_count(&r.sim), "pair %u,%u: every write counted", i, j);
                    if (r.gate.state == T4A_STOP_FAILED)
                        CHECK(r.gate.stop_step <= 3, "pair %u,%u: stop step kept", i, j);
                }
    CHECK(seen[T4A_STOP_FAILED] > 0, "two faults reach STOP_FAILED");
    printf("fault pairs: armed %u, stopped %u, stop failed %u, refused %u, hung %u\n", seen[T4A_ARMED], seen[T4A_STOPPED],
           seen[T4A_STOP_FAILED], seen[T4A_REFUSED], seen[T4A_HUNG]);
}

static void stop_step_failures(void)
{
    for (unsigned step = 0; step < 3; step++) {
        rig r;
        rig_init(&r, 5);
        reach(&r, T4A_CONFIG_SET);
        unsigned base = write_count(&r.sim);
        t4a_sim_inject(&r.sim, r.sim.accesses + step, T4A_FAULT_BUS);
        CHECK(t4a_gate_write(&r.gate, &r.io, 0x1234, 32, 0) == T4A_REFUSED_RUN, "refusal at stop step %u", step);
        CHECK(r.gate.state == T4A_STOP_FAILED && r.gate.stop_step == step && write_count(&r.sim) == base + step + 1,
              "stop step %u fails: STOP_FAILED, step kept, later stop writes not issued", step);
    }
}

static void random_requests(void)
{
    uint64_t x = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 20000; i++) {
        rig r;
        uint32_t address[6], value[6];
        rig_init(&r, 5);
        plan(&r, address, value);
        reach(&r, (enum t4a_state[]){T4A_INIT, T4A_CHECKED, T4A_PTV_SET, T4A_CONFIG_SET, T4A_ARMED}[i % 5]);
        for (int k = 0; k < 4; k++) {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
            uint32_t a = (x & 1) ? address[(x >> 8) % 6] + ((x >> 20) % 3) * 4 : (uint32_t)(x >> 16);
            uint64_t v = (x & 2) ? value[(x >> 12) % 6] ^ ((x >> 30) & 7) : x >> 24;
            t4a_gate_write(&r.gate, &r.io, a, (x & 4) ? 32 : 8 << ((x >> 40) & 3), v);
        }
        CHECK(r.sim.starts <= 1 && r.sim.stray_writes == 0 && log_is_arm_then_stop(&r), "random request sequence %d", i);
    }
}

int main(void)
{
    nominal();
    parameters();
    matrix();
    unsettled();
    terminal();
    start_only_once();
    stop_step_failures();
    fault_injection();
    fault_pairs();
    random_requests();
    printf("t4a0 gate: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
