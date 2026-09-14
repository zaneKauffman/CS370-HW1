#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "rbtree.h"

#define KEYSPACE 200

static void mkkey(char *buf, size_t bufsz, int i) {
    snprintf(buf, bufsz, "k%03d", i);
}

/* Prints a formatted failure message, tears down t (NULL-safe), and returns
 * EXIT_FAILURE -- centralizes the destroy+report pattern shared by every
 * failure site below. */
static int fail(rbtree_t *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    rb_destroy(t);
    return EXIT_FAILURE;
}

typedef struct {
    void * const *shadow;
    int visited[KEYSPACE];
    size_t count;
    int ok;
    char bad_key[16];
    void *bad_expected;
    void *bad_actual;
} cross_check_ctx_t;

/* rb_foreach callback: cross-checks one (key, value) pair from the real tree
 * against the shadow reference model. atoi(key+1) parses the numeric suffix
 * back out of "k%03d" -- sound because mkkey/idx form a bijection over
 * [0, KEYSPACE), so every key the tree can legitimately contain round-trips. */
static void cross_check_visit(const char *key, void *value, void *ctx) {
    cross_check_ctx_t *cc = ctx;
    cc->count++;
    int idx = atoi(key + 1);
    if (idx < 0 || idx >= KEYSPACE || cc->visited[idx]) {
        if (cc->ok) {
            snprintf(cc->bad_key, sizeof cc->bad_key, "%s", key);
        }
        cc->ok = 0;
        return;
    }
    cc->visited[idx] = 1;
    if (value != cc->shadow[idx]) {
        if (cc->ok) {
            snprintf(cc->bad_key, sizeof cc->bad_key, "%s", key);
            cc->bad_expected = cc->shadow[idx];
            cc->bad_actual = value;
        }
        cc->ok = 0;
    }
}

/* Walks the real tree in-order via rb_foreach and cross-checks it against
 * shadow[]: every visited node must match its shadow entry, every
 * shadow-non-NULL slot must have been visited, and the visited count must
 * equal both rb_size(t) and the count of non-NULL shadow entries.
 * Prints details and returns -1 on any mismatch, 0 on success. */
static int cross_check(const rbtree_t *t, void * const shadow[KEYSPACE], long i) {
    cross_check_ctx_t cc = {0};
    cc.shadow = shadow;
    cc.ok = 1;
    rb_foreach(t, cross_check_visit, &cc);

    size_t shadow_count = 0;
    /* invariant: scans every slot once to count expected live keys and to
     * catch a shadow entry that rb_foreach never visited (a lost insert) */
    for (int k = 0; k < KEYSPACE; k++) {
        if (shadow[k] != NULL) {
            shadow_count++;
            if (!cc.visited[k]) {
                cc.ok = 0;
            }
        }
    }

    if (!cc.ok || cc.count != rb_size(t) || cc.count != shadow_count) {
        fprintf(stderr,
            "fuzz: cross-check failed at iteration %ld: visited=%zu rb_size=%zu "
            "shadow_count=%zu bad_key=%s expected=%p actual=%p\n",
            i, cc.count, rb_size(t), shadow_count, cc.bad_key,
            cc.bad_expected, cc.bad_actual);
        return -1;
    }
    return 0;
}

/* Minimal fuzz harness: argv[1] = iteration count (default 100000).
 * Drives a pseudo-random mix of insert/find/delete against one tree, backed
 * by a bounded keyspace so both hits and misses are common. Every operation
 * is cross-checked against shadow[], an in-memory reference model of which
 * keys are present and what pointer each holds; every 100 operations (and
 * once more at the end) rb_validate and a full-tree cross-check confirm the
 * real tree agrees with that model exactly. */
int main(int argc, char **argv) {
    long iters = (argc > 1) ? strtol(argv[1], NULL, 10) : 100000;

    rbtree_t *t = rb_create(free);
    if (t == NULL) {
        fprintf(stderr, "fuzz: rb_create failed\n");
        return EXIT_FAILURE;
    }

    void *shadow[KEYSPACE] = {0}; /* NULL = key currently absent from t */
    unsigned int lcg = 2463534242u;
    /* invariant: t holds only heap values owned via rb_create(free); shadow
     * mirrors exactly which keys are present and which pointer each holds,
     * without ever owning or freeing anything itself */
    for (long i = 0; i < iters; i++) {
        lcg = lcg * 1103515245u + 12345u;
        int idx = (int)((lcg >> 16) % KEYSPACE);
        int op = (int)((lcg >> 8) % 3u);
        char kb[16];
        mkkey(kb, sizeof kb, idx);

        if (op == 0) {
            void *v = malloc(1);
            if (v == NULL) {
                return fail(t, "fuzz: out of memory at iteration %ld", i);
            }
            if (rb_insert(t, kb, v) != 0) {
                free(v);
                return fail(t, "fuzz: rb_insert failed at iteration %ld", i);
            }
            shadow[idx] = v;
            void *got = rb_find(t, kb);
            if (got != v) {
                return fail(t,
                    "fuzz: post-insert find mismatch at iteration %ld key=%s expected=%p actual=%p",
                    i, kb, v, got);
            }
        } else if (op == 1) {
            void *got = rb_find(t, kb);
            if (got != shadow[idx]) {
                return fail(t,
                    "fuzz: rb_find mismatch at iteration %ld key=%s expected=%p actual=%p",
                    i, kb, shadow[idx], got);
            }
        } else {
            int rc = rb_delete(t, kb);
            int expect_hit = (shadow[idx] != NULL);
            if ((rc == 0) != expect_hit) {
                return fail(t,
                    "fuzz: rb_delete return mismatch at iteration %ld key=%s rc=%d expect_hit=%d",
                    i, kb, rc, expect_hit);
            }
            shadow[idx] = NULL; /* correct whether rc was a hit or a miss */
        }

        if ((i + 1) % 100 == 0) {
            if (rb_validate(t) != 0) {
                return fail(t, "fuzz: rb_validate failed at iteration %ld", i);
            }
            if (cross_check(t, shadow, i) != 0) {
                rb_destroy(t);
                return EXIT_FAILURE;
            }
        }
    }

    if (rb_validate(t) != 0) {
        return fail(t, "fuzz: rb_validate failed at end");
    }
    if (cross_check(t, shadow, iters) != 0) {
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
