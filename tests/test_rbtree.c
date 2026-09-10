#include <stdint.h>
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

    if (failures == 0) {
        puts("test_rbtree: all checks passed");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
