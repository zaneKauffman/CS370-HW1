#include <stdio.h>
#include <stdlib.h>

#include "rbtree.h"

#define KEYSPACE 200

static void mkkey(char *buf, size_t bufsz, int i) {
    snprintf(buf, bufsz, "k%03d", i);
}

/* Minimal fuzz harness: argv[1] = iteration count.
 * Drives a pseudo-random mix of insert/find/delete against one tree, backed
 * by a bounded keyspace so both hits and misses are common, and validates
 * periodically to catch a corrupted tree as close as possible to the
 * mutation that caused it without making every iteration O(log n) on top of
 * the O(n) rb_validate walk. */
int main(int argc, char **argv) {
    long iters = (argc > 1) ? strtol(argv[1], NULL, 10) : 1000;

    rbtree_t *t = rb_create(free);
    if (t == NULL) {
        fprintf(stderr, "fuzz: rb_create failed\n");
        return EXIT_FAILURE;
    }

    unsigned int lcg = 2463534242u;
    /* invariant: t holds only heap values owned via rb_create(free); every
     * inserted value either gets freed by a later delete/overwrite/destroy */
    for (long i = 0; i < iters; i++) {
        lcg = lcg * 1103515245u + 12345u;
        int idx = (int)((lcg >> 16) % KEYSPACE);
        int op = (int)((lcg >> 8) % 3u);
        char kb[16];
        mkkey(kb, sizeof kb, idx);

        if (op == 0) {
            void *v = malloc(1);
            if (v == NULL) {
                fprintf(stderr, "fuzz: out of memory at iteration %ld\n", i);
                rb_destroy(t);
                return EXIT_FAILURE;
            }
            if (rb_insert(t, kb, v) != 0) {
                free(v);
                fprintf(stderr, "fuzz: rb_insert failed at iteration %ld\n", i);
                rb_destroy(t);
                return EXIT_FAILURE;
            }
        } else if (op == 1) {
            rb_find(t, kb);
        } else {
            rb_delete(t, kb); /* -1 on a miss is expected and fine */
        }

        if (i % 50 == 0) {
            if (rb_validate(t) != 0) {
                fprintf(stderr, "fuzz: rb_validate failed at iteration %ld\n", i);
                rb_destroy(t);
                return EXIT_FAILURE;
            }
        }
    }

    if (rb_validate(t) != 0) {
        fprintf(stderr, "fuzz: rb_validate failed at end\n");
        rb_destroy(t);
        return EXIT_FAILURE;
    }
    rb_destroy(t);

    /* one more pass of plain create/destroy cycles, keeping the original
     * allocation-path coverage this harness started with */
    for (long i = 0; i < 1000; i++) {
        rbtree_t *empty = rb_create(NULL);
        if (empty == NULL) {
            fprintf(stderr, "fuzz: rb_create failed in create/destroy pass\n");
            return EXIT_FAILURE;
        }
        rb_destroy(empty);
    }

    printf("fuzz: %ld insert/find/delete ops ok, keyspace %d\n", iters, KEYSPACE);
    return EXIT_SUCCESS;
}
