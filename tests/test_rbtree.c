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

    if (failures == 0) {
        puts("test_rbtree: all checks passed");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
