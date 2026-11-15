/* The console's engine benches (cp_bench.h). Not built in yet: each key only says so, and the
 * scan task restores the production settings and relearns as after any bench. */
#include "cp_bench.h"
#include <stdio.h>

void cp_bench_run(cp_t *cp, char key, cp_yield_t *y)
{
    (void)cp;
    (void)y;
    printf("CHECKPOINT bench=%c not yet: the engine benches are not in this build\n", key);
}
