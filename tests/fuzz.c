#include <stdio.h>
#include <stdlib.h>

#include "rbtree.h"

/* Minimal fuzz harness: argv[1] = iteration count.
 * Until rb_insert/rb_find/rb_delete exist, this exercises only the
 * create/destroy allocation path; it will grow to drive random operations. */
int main(int argc, char **argv) {
    long iters = (argc > 1) ? strtol(argv[1], NULL, 10) : 1000;

    /* invariant: every tree created in the loop body is destroyed before the
     * next iteration, so no state leaks across iterations */
    for (long i = 0; i < iters; i++) {
        rbtree_t *t = rb_create(NULL);
        if (t == NULL) {
            fprintf(stderr, "fuzz: rb_create failed at iteration %ld\n", i);
            return EXIT_FAILURE;
        }
        rb_destroy(t);
    }

    printf("fuzz: %ld create/destroy cycles ok\n", iters);
    return EXIT_SUCCESS;
}
