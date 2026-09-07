#include <stdio.h>
#include <stdlib.h>

#include "rbtree.h"

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                             \
            failures++;                                                 \
        }                                                              \
    } while (0)

/* Owning free fn that also records how many times it ran. */
static int value_frees;
static void counting_free(void *v) {
    free(v);
    value_frees++;
}

int main(void) {
    /* create/destroy with no value ownership */
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    rb_destroy(t);

    /* create/destroy with an owning free fn; an empty tree owns no values,
     * so the callback must never fire */
    t = rb_create(counting_free);
    CHECK(t != NULL);
    rb_destroy(t);
    CHECK(value_frees == 0);

    /* NULL-safe per the contract */
    rb_destroy(NULL);

    if (failures == 0) {
        puts("test_rbtree: all checks passed");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
