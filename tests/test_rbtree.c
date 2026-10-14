/* pthread_attr_setstack is POSIX, hidden by strict -std=c23 otherwise */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rbtree.h"
#include "fault_alloc.h"
#include "../src/pool.h"
#include <pthread.h>
#include <stdalign.h>
#if __has_include(<valgrind/memcheck.h>)
#include <valgrind/memcheck.h> /* client requests: no-ops outside valgrind */
#else
#define VALGRIND_MAKE_MEM_DEFINED(addr, len) 0
#endif

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

/* A distinct heap value; contents are irrelevant, only the pointer identity. */
static void *heap_val(void) {
    void *p = malloc(1);
    if (p == NULL) {
        fprintf(stderr, "FAIL: out of memory in test\n");
        exit(EXIT_FAILURE);
    }
    return p;
}

/* "k000".."k999" into caller's buffer (>= 5 bytes). */
static void mkkey(char *buf, size_t bufsz, int i) {
    snprintf(buf, bufsz, "k%03d", i);
}

/* rb_foreach context: tracks in-order-ness, visit count, and a value checksum. */
typedef struct {
    const char *last_key; /* greatest key seen so far, NULL before the first */
    int count;             /* real nodes visited so far */
    int ordered;           /* 1 while strictly increasing so far */
    long sum;               /* sum of values, interpreted as intptr_t */
} foreach_ctx_t;

static void foreach_visit(const char *key, void *value, void *ctx) {
    foreach_ctx_t *fc = ctx;
    if (fc->last_key != NULL && strcmp(fc->last_key, key) >= 0) {
        fc->ordered = 0;
    }
    fc->last_key = key;
    fc->count++;
    fc->sum += (long)(intptr_t)value;
}

/* In-order (key, value) record of a tree, for before/after comparison. */
#define SNAP_MAX 64
typedef struct {
    char keys[SNAP_MAX][16]; /* copies: the tree's key pointers may be freed */
    void *values[SNAP_MAX];
    int count;               /* real nodes visited */
    int overflow;            /* 1 if the tree had more than SNAP_MAX nodes */
} snapshot_t;

static void snapshot_visit(const char *key, void *value, void *ctx) {
    snapshot_t *s = ctx;
    if (s->count >= SNAP_MAX) {
        s->overflow = 1;
        return;
    }
    snprintf(s->keys[s->count], sizeof s->keys[0], "%s", key);
    s->values[s->count] = value;
    s->count++;
}

static void take_snapshot(const rbtree_t *t, snapshot_t *s) {
    memset(s, 0, sizeof *s);
    rb_foreach(t, snapshot_visit, s);
}

/* 1 iff both snapshots hold the same keys and value pointers, in order. */
static int snapshot_equal(const snapshot_t *a, const snapshot_t *b) {
    if (a->overflow || b->overflow || a->count != b->count) {
        return 0;
    }
    /* invariant: entries [0, i) already matched */
    for (int i = 0; i < a->count; i++) {
        if (strcmp(a->keys[i], b->keys[i]) != 0 || a->values[i] != b->values[i]) {
            return 0;
        }
    }
    return 1;
}

/* Caller-owned value with known contents, so a failed insert writing to it
 * is detectable. Plain malloc: does not count toward fault_alloc_live(). */
#define PATTERN_LEN 16
#define PATTERN_BYTE 0xA5
static void *patterned_val(void) {
    unsigned char *p = malloc(PATTERN_LEN);
    if (p == NULL) {
        fprintf(stderr, "FAIL: out of memory in test\n");
        exit(EXIT_FAILURE);
    }
    memset(p, PATTERN_BYTE, PATTERN_LEN);
    return p;
}

/* Mutation 3 stress: build, walk, and destroy a >= 10^6-node pooled tree
 * entirely on a deliberately small (SMALL_STACK) pthread stack. The main
 * thread only blocks in pthread_join meanwhile, so the non-atomic test and
 * fault_alloc globals are never touched concurrently. */
#define BIG_N 1000000
#define BIG_STRIDE 7919 /* prime, coprime to BIG_N: i*STRIDE % N permutes */
#define SMALL_STACK (64 * 1024)

static void *small_stack_worker(void *arg) {
    (void)arg;
    rbtree_t *bt = rb_create_pooled(NULL);
    CHECK(bt != NULL);
    if (bt == NULL) {
        return NULL;
    }
    /* invariant: keys for the first i permutation entries are inserted */
    for (long i = 0; i < BIG_N; i++) {
        long idx = i * BIG_STRIDE % BIG_N;
        char kb[16];
        snprintf(kb, sizeof kb, "m%07ld", idx);
        if (rb_insert(bt, kb, (void *)(intptr_t)1) != 0) {
            CHECK(!"insert failed in small-stack build");
            break;
        }
    }
    CHECK(rb_size(bt) == BIG_N);

    foreach_ctx_t bc = {0};
    bc.ordered = 1;
    rb_foreach(bt, foreach_visit, &bc);
    CHECK(bc.ordered == 1);
    CHECK(bc.count == BIG_N);
    CHECK(bc.sum == BIG_N);

    rb_destroy(bt);
    return NULL;
}

/* Stack-watermark probe. A balanced 10^6-node tree is only ~20-40 levels
 * deep, so recursion cannot overflow a 64 KB stack; instead we measure how
 * much stack rb_foreach / rb_destroy actually touch. The worker runs on a
 * caller-owned stack; just before the operation it paints everything below
 * its own frame with WM_BYTE, and just after it finds the lowest repainted
 * byte. Painting in the worker (not before pthread_create) keeps thread
 * start-up -- which under ASan is deeper than the tree code -- out of the
 * measurement. O(1) auxiliary space means the mark must not grow between a
 * height-2 tree and a height->=20 tree. */
#define WM_STACK (64 * 1024)
#define WM_BYTE 0xCD
#define WM_GAP 256   /* left unpainted below the painter's frame */
#define WM_CALIB 2048 /* stack the calibration job provably touches */
/* Allowed growth from 3 to 10^6 nodes: absorbs free()-path variance
 * (measured: -8 B plain, +208 B ASan) while staying below what 18+ extra
 * recursive frames cost (measured: >= 544 B plain, >= 1216 B ASan). */
#ifdef __SANITIZE_ADDRESS__
#define WM_SLACK 512
#else
#define WM_SLACK 128
#endif

typedef struct {
    rbtree_t *t;
    int op;               /* 0: rb_foreach(t), 1: rb_destroy(t), 2: calibrate */
    unsigned char *stk;   /* lowest address of the worker's stack */
    size_t used;          /* out: bytes below the painted top touched by op */
} wm_job_t;

static void wm_count(const char *key, void *value, void *ctx) {
    (void)key;
    (void)value;
    (*(long *)ctx)++;
}

/* Paints [stk, own frame - WM_GAP) and returns its length. Byte loop, not
 * memset, and unsanitized: ASan may still hold poisoned shadow for dead
 * frames down there, and an intercepted memset would check it. */
[[gnu::noinline, gnu::no_sanitize_address]]
static size_t wm_paint(volatile unsigned char *stk) {
    size_t n = (size_t)((unsigned char *)__builtin_frame_address(0) -
                        (unsigned char *)stk) - WM_GAP;
    /* invariant: stk[0..i) hold WM_BYTE */
    for (size_t i = 0; i < n; i++) {
        stk[i] = WM_BYTE;
    }
    return n;
}

/* Bytes of the painted stk[0..n) overwritten since, counted from the top
 * (the stack grows down). Unsanitized for the same reason as wm_paint; and
 * memcheck marks popped frames unaddressable, so re-mark this (heap-owned)
 * region before the deliberate read. */
[[gnu::noinline, gnu::no_sanitize_address]]
static size_t wm_touched(const volatile unsigned char *stk, size_t n) {
    (void)VALGRIND_MAKE_MEM_DEFINED(stk, n);
    size_t i = 0;
    /* invariant: stk[0..i) all still hold WM_BYTE */
    while (i < n && stk[i] == WM_BYTE) {
        i++;
    }
    return n - i;
}

/* Calibration op: provably writes WM_CALIB bytes of its own frame, so a
 * probe that fails to see stack use is caught rather than passing vacuously. */
[[gnu::noinline]]
static void wm_calibrate(void) {
    volatile unsigned char buf[WM_CALIB];
    /* invariant: buf[0..i) written */
    for (size_t i = 0; i < sizeof buf; i++) {
        buf[i] = 0;
    }
}

static void *wm_worker(void *arg) {
    wm_job_t *job = arg;
    size_t painted = wm_paint(job->stk);
    if (job->op == 2) {
        wm_calibrate();
    } else if (job->op == 1) {
        rb_destroy(job->t);
    } else {
        long n = 0;
        rb_foreach(job->t, wm_count, &n);
        CHECK((size_t)n == rb_size(job->t));
    }
    job->used = wm_touched(job->stk, painted);
    return NULL;
}

/* Runs job on a WM_STACK-byte caller-owned stack; returns the bytes its
 * operation touched, or 0 (with a CHECK failure) if the thread could not run. */
static size_t wm_measure(wm_job_t *job) {
    unsigned char *stk = aligned_alloc(4096, WM_STACK);
    CHECK(stk != NULL);
    if (stk == NULL) {
        return 0;
    }
    pthread_attr_t attr;
    pthread_t th;
    size_t used = 0;
    job->stk = stk;
    job->used = 0;
    CHECK(pthread_attr_init(&attr) == 0);
    CHECK(pthread_attr_setstack(&attr, stk, WM_STACK) == 0);
    int rc = pthread_create(&th, &attr, wm_worker, job);
    CHECK(rc == 0);
    if (rc == 0) {
        CHECK(pthread_join(th, NULL) == 0);
        used = job->used;
    }
    pthread_attr_destroy(&attr);
    free(stk);
    return used;
}

/* Pooled tree of n keys "w%07ld", inserted in a permuted order. */
static rbtree_t *wm_build(long n) {
    rbtree_t *t = rb_create_pooled(NULL);
    CHECK(t != NULL);
    if (t == NULL) {
        return NULL;
    }
    /* invariant: the first i permutation entries are inserted */
    for (long i = 0; i < n; i++) {
        char kb[24]; /* "w" + up to 19 digits of a long + NUL */
        snprintf(kb, sizeof kb, "w%07ld", i * BIG_STRIDE % n);
        CHECK(rb_insert(t, kb, NULL) == 0);
    }
    CHECK(rb_size(t) == (size_t)n);
    return t;
}

static int pattern_intact(const void *p) {
    const unsigned char *b = p;
    /* invariant: bytes [0, i) still hold PATTERN_BYTE */
    for (int i = 0; i < PATTERN_LEN; i++) {
        if (b[i] != PATTERN_BYTE) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    /* ---- create/destroy, no value ownership ---- */
    rbtree_t *t = rb_create(NULL);
    CHECK(t != NULL);
    CHECK(rb_validate(t) == 0);
    CHECK(rb_size(t) == 0);
    rb_destroy(t);

    /* ---- create/destroy with an owning free fn; empty tree owns nothing ---- */
    t = rb_create(counting_free);
    CHECK(t != NULL);
    CHECK(rb_validate(t) == 0);
    rb_destroy(t);
    CHECK(value_frees == 0);

    /* ---- NULL-safety ---- */
    rb_destroy(NULL);
    CHECK(rb_validate(NULL) != 0);
    CHECK(rb_size(NULL) == 0);

    /* ---- single insert / find ---- */
    t = rb_create(NULL);
    void *v = heap_val();
    CHECK(rb_insert(t, "hello", v) == 0);
    CHECK(rb_size(t) == 1);
    CHECK(rb_validate(t) == 0);
    CHECK(rb_find(t, "hello") == v);
    CHECK(rb_find(t, "nope") == NULL);   /* absent key */
    free(v);                             /* non-owning tree: test still owns v */
    rb_destroy(t);

    /* ---- NULL key / NULL tree: -1, tree unchanged, value not consumed ---- */
    t = rb_create(counting_free);
    void *nv = heap_val();
    CHECK(rb_insert(t, NULL, nv) == -1);
    CHECK(rb_insert(NULL, "k", nv) == -1);
    CHECK(rb_size(t) == 0);
    CHECK(rb_find(t, NULL) == NULL);
    free(nv);                            /* still ours */
    rb_destroy(t);
    CHECK(value_frees == 0);             /* nothing was ever owned */

    /* ---- ownership on success: every inserted value freed exactly once ---- */
    value_frees = 0;
    t = rb_create(counting_free);
    for (int i = 0; i < 50; i++) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_insert(t, kb, heap_val()) == 0);
    }
    CHECK(rb_size(t) == 50);
    CHECK(rb_validate(t) == 0);
    rb_destroy(t);
    CHECK(value_frees == 50);

    /* ---- overwrite, owning: old value freed, new value stored ---- */
    value_frees = 0;
    t = rb_create(counting_free);
    void *v1 = heap_val();
    void *v2 = heap_val();
    CHECK(rb_insert(t, "k", v1) == 0);
    CHECK(rb_insert(t, "k", v2) == 0);
    CHECK(value_frees == 1);             /* v1 freed on overwrite */
    CHECK(rb_size(t) == 1);              /* no new node */
    CHECK(rb_validate(t) == 0);
    CHECK(rb_find(t, "k") == v2);
    rb_destroy(t);
    CHECK(value_frees == 2);             /* v2 freed on destroy */

    /* ---- overwrite, non-owning: pointer swapped, nothing freed by tree ---- */
    t = rb_create(NULL);
    void *w1 = heap_val();
    void *w2 = heap_val();
    CHECK(rb_insert(t, "k", w1) == 0);
    CHECK(rb_insert(t, "k", w2) == 0);
    CHECK(rb_size(t) == 1);
    CHECK(rb_find(t, "k") == w2);
    free(w1);
    free(w2);
    rb_destroy(t);

    /* ---- overwrite where the old value was NULL: value_free not called ---- */
    value_frees = 0;
    t = rb_create(counting_free);
    CHECK(rb_insert(t, "k", NULL) == 0);
    void *nn = heap_val();
    CHECK(rb_insert(t, "k", nn) == 0);
    CHECK(value_frees == 0);             /* guard: no value_free(NULL) */
    rb_destroy(t);
    CHECK(value_frees == 1);             /* nn freed on destroy */

    /* ---- ascending insert: validate after every step ---- */
    t = rb_create(NULL);
    for (int i = 0; i < 100; i++) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_insert(t, kb, (void *)(intptr_t)(i + 1)) == 0);
        CHECK(rb_validate(t) == 0);
    }
    CHECK(rb_size(t) == 100);
    for (int i = 0; i < 100; i++) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_find(t, kb) == (void *)(intptr_t)(i + 1));
    }
    /* duplicate mid-tree: size stays, value updates */
    CHECK(rb_insert(t, "k042", (void *)(intptr_t)999) == 0);
    CHECK(rb_size(t) == 100);
    CHECK(rb_validate(t) == 0);
    CHECK(rb_find(t, "k042") == (void *)(intptr_t)999);
    rb_destroy(t);

    /* ---- descending insert: exercises the mirror fixup branch ---- */
    t = rb_create(NULL);
    for (int i = 99; i >= 0; i--) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_insert(t, kb, (void *)(intptr_t)(i + 1)) == 0);
        CHECK(rb_validate(t) == 0);
    }
    CHECK(rb_size(t) == 100);
    rb_destroy(t);

    /* ---- shuffled insert: deterministic LCG permutation, no srand ---- */
    t = rb_create(NULL);
    unsigned int lcg = 12345u;
    int inserted = 0;
    char seen[100] = {0};
    /* invariant: `inserted` counts distinct keys placed so far; the loop runs
     * until all 100 have been inserted at least once */
    while (inserted < 100) {
        lcg = lcg * 1103515245u + 12345u;
        int idx = (int)((lcg >> 16) % 100u);
        char kb[16];
        mkkey(kb, sizeof kb, idx);
        CHECK(rb_insert(t, kb, (void *)(intptr_t)(idx + 1)) == 0);
        CHECK(rb_validate(t) == 0);
        if (!seen[idx]) {
            seen[idx] = 1;
            inserted++;
        }
    }
    CHECK(rb_size(t) == 100);
    for (int i = 0; i < 100; i++) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_find(t, kb) == (void *)(intptr_t)(i + 1));
    }
    rb_destroy(t);

    /* ---- delete from empty tree: -1, nothing changes ---- */
    t = rb_create(NULL);
    CHECK(rb_delete(t, "x") == -1);
    CHECK(rb_size(t) == 0);
    CHECK(rb_validate(t) == 0);
    rb_destroy(t);

    /* ---- delete NULL-arg safety ---- */
    t = rb_create(NULL);
    CHECK(rb_insert(t, "a", (void *)(intptr_t)1) == 0);
    CHECK(rb_delete(NULL, "a") == -1);
    CHECK(rb_delete(t, NULL) == -1);
    CHECK(rb_size(t) == 1);
    CHECK(rb_find(t, "a") == (void *)(intptr_t)1);
    rb_destroy(t);

    /* ---- delete nonexistent key from non-empty tree: -1, unchanged ---- */
    t = rb_create(NULL);
    for (int i = 0; i < 10; i++) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_insert(t, kb, (void *)(intptr_t)(i + 1)) == 0);
    }
    CHECK(rb_delete(t, "absent") == -1);
    CHECK(rb_size(t) == 10);
    CHECK(rb_validate(t) == 0);
    for (int i = 0; i < 10; i++) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_find(t, kb) == (void *)(intptr_t)(i + 1));
    }
    rb_destroy(t);

    /* ---- delete the only node: root/leaf, back to empty ---- */
    t = rb_create(NULL);
    CHECK(rb_insert(t, "solo", (void *)(intptr_t)1) == 0);
    CHECK(rb_delete(t, "solo") == 0);
    CHECK(rb_size(t) == 0);
    CHECK(rb_validate(t) == 0);
    CHECK(rb_find(t, "solo") == NULL);
    rb_destroy(t);

    /* ---- one-child shapes: z->right == nil, then z->left == nil ---- */
    t = rb_create(NULL);
    CHECK(rb_insert(t, "k1", (void *)(intptr_t)1) == 0);
    CHECK(rb_insert(t, "k2", (void *)(intptr_t)2) == 0);
    /* "k1" is black root with a single red right child "k2" */
    CHECK(rb_delete(t, "k1") == 0);
    CHECK(rb_validate(t) == 0);
    CHECK(rb_size(t) == 1);
    CHECK(rb_find(t, "k2") == (void *)(intptr_t)2);
    rb_destroy(t);

    t = rb_create(NULL);
    CHECK(rb_insert(t, "k2", (void *)(intptr_t)2) == 0);
    CHECK(rb_insert(t, "k1", (void *)(intptr_t)1) == 0);
    /* "k2" is black root with a single red left child "k1" */
    CHECK(rb_delete(t, "k2") == 0);
    CHECK(rb_validate(t) == 0);
    CHECK(rb_size(t) == 1);
    CHECK(rb_find(t, "k1") == (void *)(intptr_t)1);
    rb_destroy(t);

    /* ---- two-children shapes: successor is z's immediate right child ---- */
    t = rb_create(NULL);
    CHECK(rb_insert(t, "k2", (void *)(intptr_t)2) == 0);
    CHECK(rb_insert(t, "k1", (void *)(intptr_t)1) == 0);
    CHECK(rb_insert(t, "k3", (void *)(intptr_t)3) == 0);
    /* delete root "k2": successor is "k3", its own immediate right child */
    CHECK(rb_delete(t, "k2") == 0);
    CHECK(rb_validate(t) == 0);
    CHECK(rb_size(t) == 2);
    CHECK(rb_find(t, "k1") == (void *)(intptr_t)1);
    CHECK(rb_find(t, "k3") == (void *)(intptr_t)3);
    CHECK(rb_find(t, "k2") == NULL);
    rb_destroy(t);

    /* ---- two-children shapes: successor deeper in z's right subtree ---- */
    t = rb_create(NULL);
    CHECK(rb_insert(t, "k4", (void *)(intptr_t)4) == 0);
    CHECK(rb_insert(t, "k2", (void *)(intptr_t)2) == 0);
    CHECK(rb_insert(t, "k6", (void *)(intptr_t)6) == 0);
    CHECK(rb_insert(t, "k1", (void *)(intptr_t)1) == 0);
    CHECK(rb_insert(t, "k3", (void *)(intptr_t)3) == 0);
    CHECK(rb_insert(t, "k5", (void *)(intptr_t)5) == 0);
    CHECK(rb_insert(t, "k7", (void *)(intptr_t)7) == 0);
    CHECK(rb_validate(t) == 0);
    /* delete root "k4": successor is "k5", not an immediate right child */
    CHECK(rb_delete(t, "k4") == 0);
    CHECK(rb_validate(t) == 0);
    CHECK(rb_size(t) == 6);
    CHECK(rb_find(t, "k4") == NULL);
    CHECK(rb_find(t, "k1") == (void *)(intptr_t)1);
    CHECK(rb_find(t, "k2") == (void *)(intptr_t)2);
    CHECK(rb_find(t, "k3") == (void *)(intptr_t)3);
    CHECK(rb_find(t, "k5") == (void *)(intptr_t)5);
    CHECK(rb_find(t, "k6") == (void *)(intptr_t)6);
    CHECK(rb_find(t, "k7") == (void *)(intptr_t)7);
    rb_destroy(t);

    /* ---- ownership on delete: value_free called exactly once per removed
     * key, not called on destroy for the ones already removed ---- */
    value_frees = 0;
    t = rb_create(counting_free);
    for (int i = 0; i < 50; i++) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_insert(t, kb, heap_val()) == 0);
    }
    for (int i = 0; i < 20; i++) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_delete(t, kb) == 0);
    }
    CHECK(value_frees == 20);
    CHECK(rb_size(t) == 30);
    CHECK(rb_validate(t) == 0);
    rb_destroy(t);
    CHECK(value_frees == 50);

    /* ---- delete where the value was NULL: value_free not called ---- */
    value_frees = 0;
    t = rb_create(counting_free);
    CHECK(rb_insert(t, "k", NULL) == 0);
    CHECK(rb_delete(t, "k") == 0);
    CHECK(value_frees == 0); /* guard: no value_free(NULL) */
    rb_destroy(t);

    /* ---- non-owning tree: delete doesn't free values the test still owns;
     * asan/valgrind (not CHECK) prove no double-free here ---- */
    t = rb_create(NULL);
    void *dv1 = heap_val();
    void *dv2 = heap_val();
    CHECK(rb_insert(t, "d1", dv1) == 0);
    CHECK(rb_insert(t, "d2", dv2) == 0);
    CHECK(rb_delete(t, "d1") == 0);
    CHECK(rb_delete(t, "d2") == 0);
    CHECK(rb_size(t) == 0);
    free(dv1);
    free(dv2);
    rb_destroy(t);

    /* ---- bulk coverage: shuffled insert, then independently-shuffled
     * delete of everything, validating after every single delete. This is
     * the practical black-box way to exercise all rb_delete_fixup cases and
     * both mirror sides, since the public API can't target node colors. ---- */
    value_frees = 0;
    t = rb_create(counting_free);
    unsigned int dlcg = 987654321u;
    int dinserted = 0;
    char dseen[150] = {0};
    /* invariant: dinserted counts distinct keys placed so far */
    while (dinserted < 150) {
        dlcg = dlcg * 1103515245u + 12345u;
        int idx = (int)((dlcg >> 16) % 150u);
        char kb[16];
        mkkey(kb, sizeof kb, idx);
        if (!dseen[idx]) {
            CHECK(rb_insert(t, kb, heap_val()) == 0);
            dseen[idx] = 1;
            dinserted++;
        }
    }
    CHECK(rb_size(t) == 150);
    CHECK(rb_validate(t) == 0);

    /* second, independent shuffle order for deletion */
    unsigned int dlcg2 = 246813579u;
    int ddeleted = 0;
    char ddone[150] = {0};
    /* invariant: ddeleted counts distinct keys removed so far */
    while (ddeleted < 150) {
        dlcg2 = dlcg2 * 1103515245u + 12345u;
        int idx = (int)((dlcg2 >> 16) % 150u);
        if (!ddone[idx]) {
            char kb[16];
            mkkey(kb, sizeof kb, idx);
            CHECK(rb_delete(t, kb) == 0);
            CHECK(rb_validate(t) == 0);
            CHECK(rb_find(t, kb) == NULL);
            ddone[idx] = 1;
            ddeleted++;
        }
    }
    CHECK(rb_size(t) == 0);
    CHECK(rb_validate(t) == 0);
    rb_destroy(t);
    CHECK(value_frees == 150);

    /* ---- repeated insert/delete/reinsert cycles ---- */
    t = rb_create(NULL);
    for (int i = 0; i < 50; i++) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_insert(t, kb, (void *)(intptr_t)(i + 1)) == 0);
    }
    CHECK(rb_validate(t) == 0);
    for (int i = 0; i < 50; i += 2) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_delete(t, kb) == 0);
    }
    CHECK(rb_size(t) == 25);
    CHECK(rb_validate(t) == 0);
    for (int i = 0; i < 50; i += 2) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_insert(t, kb, (void *)(intptr_t)(i + 100)) == 0);
    }
    CHECK(rb_size(t) == 50);
    CHECK(rb_validate(t) == 0);
    for (int i = 0; i < 50; i++) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_delete(t, kb) == 0);
    }
    CHECK(rb_size(t) == 0);
    CHECK(rb_validate(t) == 0);
    rb_destroy(t);

    /* ---- rb_foreach on an empty tree: no visits ---- */
    t = rb_create(NULL);
    foreach_ctx_t fc = {0};
    fc.ordered = 1;
    rb_foreach(t, foreach_visit, &fc);
    CHECK(fc.count == 0);
    CHECK(fc.ordered == 1);

    /* ---- rb_foreach NULL-safety: neither call visits anything ---- */
    CHECK(rb_insert(t, "a", (void *)(intptr_t)1) == 0);
    rb_foreach(NULL, foreach_visit, &fc);
    CHECK(fc.count == 0);
    rb_foreach(t, NULL, &fc);
    CHECK(fc.count == 0);
    rb_destroy(t);

    /* ---- rb_foreach: sorted order, full coverage, value pass-through ---- */
    t = rb_create(NULL);
    unsigned int flcg = 555555555u;
    int finserted = 0;
    char fseen[100] = {0};
    /* invariant: finserted counts distinct keys placed so far */
    while (finserted < 100) {
        flcg = flcg * 1103515245u + 12345u;
        int idx = (int)((flcg >> 16) % 100u);
        char kb[16];
        mkkey(kb, sizeof kb, idx);
        CHECK(rb_insert(t, kb, (void *)(intptr_t)(idx + 1)) == 0);
        if (!fseen[idx]) {
            fseen[idx] = 1;
            finserted++;
        }
    }
    CHECK(rb_size(t) == 100);
    foreach_ctx_t fc2 = {0};
    fc2.ordered = 1;
    rb_foreach(t, foreach_visit, &fc2);
    CHECK(fc2.ordered == 1);
    CHECK((size_t)fc2.count == rb_size(t));
    CHECK(fc2.sum == 5050); /* sum of 1..100 */

    /* ---- rb_foreach reflects post-delete state ---- */
    for (int i = 0; i < 100; i += 3) {
        char kb[16];
        mkkey(kb, sizeof kb, i);
        CHECK(rb_delete(t, kb) == 0);
    }
    foreach_ctx_t fc3 = {0};
    fc3.ordered = 1;
    rb_foreach(t, foreach_visit, &fc3);
    CHECK(fc3.ordered == 1);
    CHECK((size_t)fc3.count == rb_size(t));
    rb_destroy(t);

    /* ================= allocation-failure (OOM) coverage =================
     * Every internal allocation in src/rbtree.c goes through rb_malloc, so
     * fault_alloc_arm(k) forces the k-th allocation from that point to fail.
     * The per-operation allocation counts are measured rather than hard-coded,
     * so these stay honest when Mutation 2 moves nodes into a slab pool. */
    {
        long oom_base;

        /* ---- rb_create: every allocation forced to fail yields NULL ---- */
        oom_base = fault_alloc_total();
        t = rb_create(NULL);
        CHECK(t != NULL);
        long oom_create_allocs = fault_alloc_total() - oom_base;
        rb_destroy(t);
        CHECK(oom_create_allocs >= 1); /* tree struct + nil sentinel: 2 today */

        /* invariant: k is the 1-based index of the rb_create allocation being
         * failed; k == oom_create_allocs exercises the fail_tree: path. */
        for (long k = 1; k <= oom_create_allocs; k++) {
            fault_alloc_arm(k);
            rbtree_t *ft = rb_create(counting_free);
            fault_alloc_disarm();
            CHECK(ft == NULL); /* contract: NULL, partial allocation released */
            rb_destroy(ft);    /* NULL-safe; a no-op unless the CHECK failed */
        }

        /* ---- measure rb_insert's cost on a fresh key ---- */
        long oom_insert_allocs;
        {
            rbtree_t *mt = rb_create(NULL); /* non-owning: probe value is ours */
            CHECK(mt != NULL);
            void *mv = heap_val();
            oom_base = fault_alloc_total();
            CHECK(rb_insert(mt, "probe", mv) == 0);
            oom_insert_allocs = fault_alloc_total() - oom_base;
            rb_destroy(mt);
            free(mv);
            CHECK(oom_insert_allocs >= 1); /* node + key copy: 2 today */
        }

        /* ---- rb_insert into an empty tree: tree unchanged, value kept ---- */
        /* invariant: k is the 1-based index of the rb_insert allocation being
         * failed; k == oom_insert_allocs exercises the fail_node: path. */
        for (long k = 1; k <= oom_insert_allocs; k++) {
            value_frees = 0;
            t = rb_create(counting_free);
            CHECK(t != NULL);
            void *v = patterned_val();
            snapshot_t before, after;
            take_snapshot(t, &before);
            long live_before = fault_alloc_live();

            fault_alloc_arm(k);
            int rc = rb_insert(t, "orphan", v);
            fault_alloc_disarm();

            CHECK(rc == -1);                     /* documented error code */
            CHECK(fault_alloc_live() == live_before); /* failed call leaked nothing */
            take_snapshot(t, &after);
            CHECK(snapshot_equal(&before, &after)); /* contents exactly as before */
            CHECK(pattern_intact(v));            /* value bytes not written */
            CHECK(rb_size(t) == 0);              /* tree unchanged */
            CHECK(rb_validate(t) == 0);          /* still a valid RB tree */
            CHECK(rb_find(t, "orphan") == NULL); /* key not present */
            CHECK(value_frees == 0);             /* value NOT consumed */
            rb_destroy(t);
            CHECK(value_frees == 0);             /* nor consumed by destroy */
            free(v);                             /* still ours to release */
        }

        /* ---- rb_insert into a populated tree: contents undisturbed ---- */
        for (long k = 1; k <= oom_insert_allocs; k++) {
            value_frees = 0;
            t = rb_create(counting_free);
            CHECK(t != NULL);
            for (int i = 0; i < 50; i++) {
                char kb[16];
                mkkey(kb, sizeof kb, i);
                CHECK(rb_insert(t, kb, heap_val()) == 0);
            }
            CHECK(rb_size(t) == 50);
            CHECK(rb_validate(t) == 0);

            void *v = patterned_val();
            snapshot_t before, after;
            take_snapshot(t, &before);
            long live_before = fault_alloc_live();

            fault_alloc_arm(k);
            int rc = rb_insert(t, "zzz", v); /* sorts after every k0NN key */
            fault_alloc_disarm();

            CHECK(rc == -1);
            CHECK(fault_alloc_live() == live_before); /* failed call leaked nothing */
            take_snapshot(t, &after);
            CHECK(snapshot_equal(&before, &after)); /* same keys, values, order */
            CHECK(pattern_intact(v));          /* value bytes not written */
            CHECK(rb_size(t) == 50);           /* no size change */
            CHECK(rb_validate(t) == 0);        /* structure still valid */
            CHECK(rb_find(t, "zzz") == NULL);  /* failed key absent */
            CHECK(value_frees == 0);           /* nothing freed on this path */

            /* the 50 pre-existing entries must survive the failed insert */
            foreach_ctx_t oc = {0};
            oc.ordered = 1;
            rb_foreach(t, foreach_visit, &oc);
            CHECK(oc.ordered == 1);
            CHECK(oc.count == 50);
            for (int i = 0; i < 50; i++) {
                char kb[16];
                mkkey(kb, sizeof kb, i);
                CHECK(rb_find(t, kb) != NULL);
            }

            rb_destroy(t);
            CHECK(value_frees == 50); /* the 50 owned values, never v */
            free(v);
        }

        /* ---- non-owning tree: failed insert leaves everything intact, and the
         * identical retry then succeeds (no half-state left behind) ---- */
        /* invariant: k is the 1-based index of the rb_insert allocation being
         * failed; the tree is rebuilt fresh for each k. */
        for (long k = 1; k <= oom_insert_allocs; k++) {
            t = rb_create(NULL);
            CHECK(t != NULL);
            for (int i = 0; i < 20; i++) {
                char kb[16];
                mkkey(kb, sizeof kb, i);
                CHECK(rb_insert(t, kb, (void *)(intptr_t)(i + 1)) == 0);
            }

            void *v = patterned_val();
            snapshot_t before, after;
            take_snapshot(t, &before);
            long live_before = fault_alloc_live();

            fault_alloc_arm(k);
            int rc = rb_insert(t, "k010x", v); /* lands mid-tree */
            fault_alloc_disarm();

            CHECK(rc == -1);
            CHECK(fault_alloc_live() == live_before);
            take_snapshot(t, &after);
            CHECK(snapshot_equal(&before, &after));
            CHECK(rb_size(t) == 20);
            CHECK(rb_validate(t) == 0);
            CHECK(rb_find(t, "k010x") == NULL);
            CHECK(pattern_intact(v));

            /* retry, disarmed: must succeed exactly as if nothing had failed */
            CHECK(rb_insert(t, "k010x", v) == 0);
            CHECK(rb_size(t) == 21);
            CHECK(rb_validate(t) == 0);
            CHECK(rb_find(t, "k010x") == v);
            CHECK(fault_alloc_live() == live_before + oom_insert_allocs);

            rb_destroy(t);
            CHECK(pattern_intact(v)); /* non-owning: destroy never touched v */
            free(v);
        }

        /* ---- overwrite allocates nothing, so arming must not break it ---- */
        value_frees = 0;
        t = rb_create(counting_free);
        CHECK(t != NULL);
        CHECK(rb_insert(t, "dup", heap_val()) == 0);
        void *onv = heap_val();
        oom_base = fault_alloc_total();
        fault_alloc_arm(1);                   /* the next allocation would fail */
        int orc = rb_insert(t, "dup", onv);   /* but overwrite makes none */
        fault_alloc_disarm();
        CHECK(orc == 0);                          /* must still succeed */
        CHECK(fault_alloc_total() == oom_base);   /* proof: zero allocations */
        CHECK(rb_size(t) == 1);
        CHECK(rb_find(t, "dup") == onv);
        CHECK(value_frees == 1);                  /* old value freed */
        rb_destroy(t);
        CHECK(value_frees == 2);

        /* ---- find/size/validate/foreach/delete allocate nothing ---- */
        t = rb_create(NULL);
        CHECK(t != NULL);
        for (int i = 0; i < 20; i++) {
            char kb[16];
            mkkey(kb, sizeof kb, i);
            CHECK(rb_insert(t, kb, (void *)(intptr_t)(i + 1)) == 0);
        }
        oom_base = fault_alloc_total();
        foreach_ctx_t rc2 = {0};
        rc2.ordered = 1;
        CHECK(rb_find(t, "k005") != NULL);
        CHECK(rb_size(t) == 20);
        CHECK(rb_validate(t) == 0);
        rb_foreach(t, foreach_visit, &rc2);
        CHECK(rb_delete(t, "k005") == 0);
        CHECK(rb_delete(t, "nope") == -1);
        CHECK(fault_alloc_total() == oom_base); /* no read/delete path allocates */
        rb_destroy(t);

        /* ---- armed out of reach: behaviour identical to disarmed ---- */
        fault_alloc_arm(1000000);
        t = rb_create(NULL);
        CHECK(t != NULL);
        for (int i = 0; i < 100; i++) {
            char kb[16];
            mkkey(kb, sizeof kb, i);
            CHECK(rb_insert(t, kb, (void *)(intptr_t)(i + 1)) == 0);
        }
        CHECK(rb_size(t) == 100);
        CHECK(rb_validate(t) == 0);
        for (int i = 0; i < 100; i += 2) {
            char kb[16];
            mkkey(kb, sizeof kb, i);
            CHECK(rb_delete(t, kb) == 0);
        }
        CHECK(rb_size(t) == 50);
        CHECK(rb_validate(t) == 0);
        rb_destroy(t);
        fault_alloc_disarm();
    }

    /* ================= slab pool (Mutation 2) ================= */
    {
        size_t ns, nl, nf;
        long live0 = fault_alloc_live();

        /* ---- argument checks and NULL safety ---- */
        CHECK(pool_create(0) == NULL);
        CHECK(pool_create(4096) == NULL); /* cannot fit beside the slab header */
        CHECK(pool_alloc(NULL) == NULL);
        pool_free(NULL, NULL);
        pool_destroy(NULL);
        pool_stats(NULL, &ns, &nl, &nf);
        CHECK(ns == 0 && nl == 0 && nf == 0);
        CHECK(fault_alloc_live() == live0);

        /* ---- pool_create OOM: NULL, nothing leaked ---- */
        fault_alloc_arm(1);
        CHECK(pool_create(40) == NULL);
        fault_alloc_disarm();
        CHECK(fault_alloc_live() == live0);

        rb_pool_t *p = pool_create(40);
        CHECK(p != NULL);
        pool_stats(p, &ns, &nl, &nf);
        CHECK(ns == 0 && nl == 0 && nf == 0); /* slabs are lazy */

        /* ---- first alloc: one slab, aligned, writable ---- */
        void *o0 = pool_alloc(p);
        CHECK(o0 != NULL);
        CHECK((uintptr_t)o0 % alignof(max_align_t) == 0);
        memset(o0, 0x5A, 40);
        pool_stats(p, &ns, &nl, &nf);
        CHECK(ns == 1 && nl == 1 && nf >= 1);
        size_t per_slab = nl + nf;
        CHECK(per_slab * 40 <= 4096);

        /* ---- grow across several slabs; all objects distinct ---- */
        enum { NOBJ = 300 };
        void *objs[NOBJ];
        objs[0] = o0;
        /* invariant: objs[0, i) are live, distinct, aligned pool objects */
        for (int i = 1; i < NOBJ; i++) {
            objs[i] = pool_alloc(p);
            CHECK(objs[i] != NULL);
            CHECK((uintptr_t)objs[i] % alignof(max_align_t) == 0);
            memset(objs[i], i & 0xFF, 40);
        }
        /* invariant: no pair in objs[0, i) overlaps */
        for (int i = 0; i < NOBJ; i++) {
            for (int j = i + 1; j < NOBJ; j++) {
                CHECK((char *)objs[i] + 40 <= (char *)objs[j] ||
                      (char *)objs[j] + 40 <= (char *)objs[i]);
            }
        }
        pool_stats(p, &ns, &nl, &nf);
        CHECK(nl == NOBJ);
        CHECK(ns == (NOBJ + per_slab - 1) / per_slab);
        CHECK(ns * per_slab == nl + nf);
        CHECK(fault_alloc_live() == live0 + 1 + (long)ns); /* pool + slabs */

        /* ---- free then alloc reuses the object with no new allocation ---- */
        long tot = fault_alloc_total();
        pool_free(p, objs[17]);
        void *re = pool_alloc(p);
        CHECK(re == objs[17]);
        CHECK(fault_alloc_total() == tot);

        /* ---- OOM when a new slab is needed: NULL, pool unchanged ---- */
        /* invariant: every call so far was served without a new slab */
        while (nf > 0) {
            CHECK(pool_alloc(p) != NULL);
            pool_stats(p, &ns, &nl, &nf);
        }
        size_t ns_full = ns, nl_full = nl;
        long live_full = fault_alloc_live();
        fault_alloc_arm(1);
        CHECK(pool_alloc(p) == NULL);
        fault_alloc_disarm();
        pool_stats(p, &ns, &nl, &nf);
        CHECK(ns == ns_full && nl == nl_full && nf == 0);
        CHECK(fault_alloc_live() == live_full);
        CHECK(pool_alloc(p) != NULL); /* retry succeeds */
        pool_stats(p, &ns, &nl, &nf);
        CHECK(ns == ns_full + 1 && nl == nl_full + 1);

        /* ---- free everything: live 0, capacity retained ---- */
        for (int i = 0; i < NOBJ; i++) {
            pool_free(p, objs[i]);
        }
        pool_stats(p, &ns, &nl, &nf);
        CHECK(nl == nl_full + 1 - NOBJ);

        pool_destroy(p);
        CHECK(fault_alloc_live() == live0); /* every slab released */
    }

    /* ================= pooled tree ================= */
    {
        long live0 = fault_alloc_live();
        long oom_base = fault_alloc_total();
        t = rb_create_pooled(NULL);
        CHECK(t != NULL);
        long pooled_create_allocs = fault_alloc_total() - oom_base;
        rb_destroy(t);
        CHECK(fault_alloc_live() == live0);

        /* invariant: k is the 1-based index of the rb_create_pooled
         * allocation being failed */
        for (long k = 1; k <= pooled_create_allocs; k++) {
            fault_alloc_arm(k);
            rbtree_t *ft = rb_create_pooled(counting_free);
            fault_alloc_disarm();
            CHECK(ft == NULL);
            CHECK(fault_alloc_live() == live0);
        }

        /* ---- randomized insert/delete; invariants checked after every op ---- */
        enum { KEYS = 300, OPS = 4000 };
        char present[KEYS] = {0};
        size_t npresent = 0;
        unsigned lcg = 12345;
        t = rb_create_pooled(NULL);
        CHECK(t != NULL);
        /* invariant: present[] and npresent mirror the tree's key set */
        for (int op = 0; op < OPS; op++) {
            lcg = lcg * 1103515245u + 12345u;
            int i = (int)((lcg >> 8) % KEYS);
            char kb[16];
            mkkey(kb, sizeof kb, i);
            if ((lcg >> 4) & 1) {
                CHECK(rb_insert(t, kb, (void *)(intptr_t)(i + 1)) == 0);
                if (!present[i]) {
                    present[i] = 1;
                    npresent++;
                }
            } else {
                CHECK(rb_delete(t, kb) == (present[i] ? 0 : -1));
                if (present[i]) {
                    present[i] = 0;
                    npresent--;
                }
            }
            CHECK(rb_validate(t) == 0);
            CHECK(rb_size(t) == npresent);
        }
        /* invariant: keys [0, i) have been checked against present[] */
        for (int i = 0; i < KEYS; i++) {
            char kb[16];
            mkkey(kb, sizeof kb, i);
            CHECK(rb_find(t, kb) == (present[i] ? (void *)(intptr_t)(i + 1) : NULL));
        }
        foreach_ctx_t pc = {0};
        pc.ordered = 1;
        rb_foreach(t, foreach_visit, &pc);
        CHECK(pc.ordered == 1);
        CHECK((size_t)pc.count == npresent);
        rb_destroy(t);
        CHECK(fault_alloc_live() == live0);

        /* ---- insert OOM into an empty pooled tree (key copy + first slab):
         * tree unchanged, value kept, nothing leaked, retry succeeds ---- */
        long pooled_insert_allocs;
        {
            rbtree_t *mt = rb_create_pooled(NULL);
            CHECK(mt != NULL);
            oom_base = fault_alloc_total();
            CHECK(rb_insert(mt, "probe", (void *)1) == 0);
            pooled_insert_allocs = fault_alloc_total() - oom_base;
            rb_destroy(mt);
            CHECK(pooled_insert_allocs >= 2); /* key copy + a fresh slab */
        }
        /* invariant: k is the 1-based index of the rb_insert allocation being
         * failed; the tree is rebuilt fresh for each k */
        for (long k = 1; k <= pooled_insert_allocs; k++) {
            value_frees = 0;
            t = rb_create_pooled(counting_free);
            CHECK(t != NULL);
            void *v = patterned_val();
            long live_before = fault_alloc_live();

            fault_alloc_arm(k);
            int rc = rb_insert(t, "orphan", v);
            fault_alloc_disarm();

            CHECK(rc == -1);
            CHECK(fault_alloc_live() == live_before);
            CHECK(pattern_intact(v));
            CHECK(rb_size(t) == 0);
            CHECK(rb_validate(t) == 0);
            CHECK(rb_find(t, "orphan") == NULL);
            CHECK(value_frees == 0);

            CHECK(rb_insert(t, "orphan", v) == 0); /* retry, disarmed */
            CHECK(rb_find(t, "orphan") == v);
            CHECK(rb_validate(t) == 0);
            rb_destroy(t);
            CHECK(value_frees == 1); /* owned after the successful retry */
            CHECK(fault_alloc_live() == live0);
        }

        /* ---- insert OOM into a populated pooled tree ---- */
        for (long k = 1; k <= pooled_insert_allocs; k++) {
            value_frees = 0;
            t = rb_create_pooled(counting_free);
            CHECK(t != NULL);
            for (int i = 0; i < 50; i++) {
                char kb[16];
                mkkey(kb, sizeof kb, i);
                CHECK(rb_insert(t, kb, heap_val()) == 0);
            }
            void *v = patterned_val();
            snapshot_t before, after;
            take_snapshot(t, &before);
            long live_before = fault_alloc_live();

            fault_alloc_arm(k);
            int rc = rb_insert(t, "k025x", v);
            fault_alloc_disarm();

            if (rc == 0) {
                /* this state needed fewer allocations than an empty tree
                 * (slab space already available); the arm went unfired */
                CHECK(rb_delete(t, "k025x") == 0);
                CHECK(value_frees == 1); /* v was consumed, then freed */
            } else {
                CHECK(fault_alloc_live() == live_before);
                take_snapshot(t, &after);
                CHECK(snapshot_equal(&before, &after));
                CHECK(pattern_intact(v));
                CHECK(rb_size(t) == 50);
                CHECK(rb_validate(t) == 0);
                CHECK(rb_find(t, "k025x") == NULL);
                CHECK(value_frees == 0);
                free(v);
            }
            CHECK(rb_validate(t) == 0);
            rb_destroy(t);
            CHECK(fault_alloc_live() == live0);
        }
    }

    /* ================= Mutation 3: iterative foreach/destroy ================= */
    {
        long live0 = fault_alloc_live();
        enum { BIG = 10000 };

        /* ---- large ascending/descending builds, plain and pooled ---- */
        /* invariant: pass covers {plain, pooled} x {ascending, descending} */
        for (int pass = 0; pass < 4; pass++) {
            rbtree_t *bt = (pass & 1) ? rb_create_pooled(counting_free)
                                      : rb_create(counting_free);
            CHECK(bt != NULL);
            value_frees = 0;
            int nonnull = 0;
            for (int i = 0; i < BIG; i++) {
                int idx = (pass & 2) ? BIG - 1 - i : i;
                char kb[16];
                snprintf(kb, sizeof kb, "b%05d", idx);
                /* every 7th value is NULL: destroy must not pass it to value_free */
                void *v = (idx % 7 == 0) ? NULL : heap_val();
                nonnull += (v != NULL);
                CHECK(rb_insert(bt, kb, v) == 0);
            }
            CHECK(rb_size(bt) == BIG);
            CHECK(rb_validate(bt) == 0);

            long tot0 = fault_alloc_total();
            foreach_ctx_t bc = {0};
            bc.ordered = 1;
            rb_foreach(bt, foreach_visit, &bc);
            CHECK(bc.ordered == 1);
            CHECK(bc.count == BIG);
            CHECK(fault_alloc_total() == tot0); /* foreach allocates nothing */
            CHECK(rb_validate(bt) == 0);        /* and leaves the tree intact */

            rb_destroy(bt);
            CHECK(value_frees == nonnull); /* each non-NULL value exactly once */
            CHECK(fault_alloc_live() == live0);
        }
        value_frees = 0;

        /* ---- single node: foreach visits it once, destroy frees it ---- */
        rbtree_t *st = rb_create(NULL);
        CHECK(st != NULL);
        CHECK(rb_insert(st, "only", (void *)(intptr_t)7) == 0);
        foreach_ctx_t sc = {0};
        sc.ordered = 1;
        rb_foreach(st, foreach_visit, &sc);
        CHECK(sc.count == 1 && sc.sum == 7);
        rb_destroy(st);
        CHECK(fault_alloc_live() == live0);

        /* ---- 10^6 nodes: build, foreach, destroy on a 64 KB stack ---- */
        pthread_attr_t attr;
        pthread_t th;
        CHECK(pthread_attr_init(&attr) == 0);
        CHECK(pthread_attr_setstacksize(&attr, SMALL_STACK) == 0);
        int prc = pthread_create(&th, &attr, small_stack_worker, NULL);
        CHECK(prc == 0);
        if (prc == 0) {
            CHECK(pthread_join(th, NULL) == 0);
        }
        pthread_attr_destroy(&attr);
        CHECK(fault_alloc_live() == live0); /* every slab and key released */

        /* ---- stack high-water mark independent of n (3 vs 10^6) ---- */
        rbtree_t *small = wm_build(3);
        rbtree_t *large = wm_build(BIG_N);
        if (small != NULL && large != NULL) {
            wm_job_t cj = {NULL, 2, NULL, 0};
            wm_job_t fs = {small, 0, NULL, 0}, fl = {large, 0, NULL, 0};
            wm_job_t ds = {small, 1, NULL, 0}, dl = {large, 1, NULL, 0};
            size_t cal = wm_measure(&cj);
            size_t fe_s = wm_measure(&fs), fe_l = wm_measure(&fl);
            size_t de_s = wm_measure(&ds), de_l = wm_measure(&dl);
            if (getenv("WM_VERBOSE") != NULL) {
                printf("watermark calib %zu, foreach %zu -> %zu, "
                       "destroy %zu -> %zu\n", cal, fe_s, fe_l, de_s, de_l);
            }
            /* the probe really observes stack use (top WM_GAP is unpainted) */
            CHECK(cal + WM_GAP >= WM_CALIB);
            CHECK(fe_l <= fe_s + WM_SLACK);
            CHECK(de_l <= de_s + WM_SLACK);
        } else {
            rb_destroy(small); /* NULL-safe */
            rb_destroy(large);
        }
        CHECK(fault_alloc_live() == live0);
    }

    if (failures == 0) {
        puts("test_rbtree: all checks passed");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
