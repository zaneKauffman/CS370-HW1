#include <stdlib.h>
#include "fault_alloc.h"

/* Fault-injecting allocator shim. Every internal allocation in the rbtree
 * library goes through rb_malloc/rb_free, so a test can force one allocation
 * to fail and drive the library's error paths on demand.
 *
 * Disarmed is the default and is a pure passthrough, so the test, asan and
 * memcheck builds all exercise the same code path the graded library uses.
 * Plain globals rather than atomics: -lpthread is linked, but nothing in this
 * repo is threaded. */

/* Allocations still to be attempted before the armed failure fires, or -1 when
 * disarmed. Reaching 0 fires once and leaves the shim disarmed. */
static long g_countdown = -1;

/* Every rb_malloc call ever made, counting the one that gets failed. */
static long g_total = 0;

/* Successful rb_malloc results not yet passed to rb_free. */
static long g_live = 0;

void *rb_malloc(size_t n) {
    g_total++;
    /* invariant: g_countdown > 0 means this many allocations, counting this
     * one, remain before the armed failure. */
    if (g_countdown > 0 && --g_countdown == 0) {
        return NULL; /* countdown is now 0, so this fires exactly once */
    }
    void *p = malloc(n);
    if (p != NULL) {
        g_live++;
    }
    return p;
}

void rb_free(void *p) {
    if (p != NULL) {
        g_live--;
    }
    free(p); /* NULL-safe, like free(); several call sites rely on that */
}

/* Arm so the n-th allocation FROM NOW returns NULL; n == 1 fails the very next
 * one. n <= 0 disarms. */
void fault_alloc_arm(long n) {
    g_countdown = (n > 0) ? n : -1;
}

void fault_alloc_disarm(void) {
    g_countdown = -1;
}

long fault_alloc_total(void) {
    return g_total;
}

long fault_alloc_live(void) {
    return g_live;
}
