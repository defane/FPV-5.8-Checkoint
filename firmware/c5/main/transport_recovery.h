/* C5VRX by Twotoz and contributors: one bounded restart + verified rollback.
 * Callbacks own SDK state and publish only after every stage succeeds. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    TR_BS_ENABLE, TR_PROGRAM_LOAD, TR_INSTRUCTION_VERIFY, TR_LUT_VERIFY,
    TR_BS_RESET, TR_BS_START, TR_RX_ENABLE, TR_TX_ENABLE, TR_RX_START,
    TR_TX_START, TR_STAGES
} tr_stage_t;
typedef enum { TR_APPLIED, TR_ROLLED_BACK, TR_LATCHED } tr_result_t;
typedef bool (*tr_clean_fn)(void *);
typedef bool (*tr_step_fn)(void *, tr_stage_t, bool);
typedef enum { TR_TX, TR_BS, TR_RX } tr_part_t;
typedef bool (*tr_stop_fn)(void *, tr_part_t);
static inline bool tr_stop_parts(void *ctx, tr_stop_fn stop, bool *tx, bool *bs, bool *rx, bool stop_rx)
{
    bool *enabled[3] = {tx, bs, rx};
    bool ok = true;
    for (int i = 0; i < (stop_rx ? 3 : 2); ++i) {
        if (!*enabled[i]) continue;
        if (stop(ctx, (tr_part_t)i)) *enabled[i] = false;
        else ok = false; /* still clean up every other enabled part */
    }
    return ok;
}

/* Optional timing (T TRANSPORT_STAGES_US, range audit PR #230): when the
 * owner sets both, note(stage, us) follows every stage, stage -1 every
 * teardown. Unset (host tests) the restart is unchanged. */
typedef int64_t (*tr_now_fn)(void);
typedef void (*tr_note_fn)(int stage, int64_t us);
static tr_now_fn tr_hook_now;
static tr_note_fn tr_hook_note;
static inline bool tr_clean_noted(void *ctx, tr_clean_fn clean)
{
    if (!tr_hook_now || !tr_hook_note) return clean(ctx);
    const int64_t t0 = tr_hook_now();
    const bool ok = clean(ctx);
    tr_hook_note(-1, tr_hook_now() - t0);
    return ok;
}
static inline bool tr_attempt(void *ctx, tr_step_fn step, bool rollback)
{
    for (int s = 0; s < TR_STAGES; ++s) {
        const int64_t t0 = tr_hook_now && tr_hook_note ? tr_hook_now() : 0;
        const bool ok = step(ctx, (tr_stage_t)s, rollback);
        if (tr_hook_now && tr_hook_note) tr_hook_note(s, tr_hook_now() - t0);
        if (!ok) return false;
    }
    return true;
}
static inline tr_result_t tr_restart(void *ctx, tr_clean_fn clean, tr_step_fn step, bool have_previous)
{
    /* Failed teardown is terminal: never enable another producer on top. */
    if (!tr_clean_noted(ctx, clean)) return TR_LATCHED;
    if (tr_attempt(ctx, step, false)) return TR_APPLIED;
    if (!tr_clean_noted(ctx, clean)) return TR_LATCHED;
    if (have_previous && tr_attempt(ctx, step, true)) return TR_ROLLED_BACK;
    (void)clean(ctx); /* final best effort; caller latches even if this fails */
    return TR_LATCHED;
}
