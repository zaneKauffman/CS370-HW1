#include <stdio.h>
#include <stdlib.h>
#include "rbtree.h"

typedef enum { RED, BLACK } rb_color_t;

typedef struct rb_node {
    char *key; // Pointer to a copy of string key
    void *value; // Pointer storing node's value
    rb_color_t color; // Node's color
    struct rb_node *parent; // Pointer towards parent node (Above)
    struct rb_node *left; // Pointer towards the smaller child node
    struct rb_node *right; // Pointer towards the larger child node
} rb_node_t;

/* The concrete definition of the opaque `rbtree_t` from rbtree.h. Callers only
 * ever hold a `struct rbtree *`, so every field here is private and may change
 * without touching the graded contract in include/rbtree.h. One of these is
 * allocated per tree by rb_create() and released by rb_destroy(). */
struct rbtree {
    /* Current root of the tree. Points at `nil` (never a raw NULL) when the tree
     * is empty, so traversal and fixup code can treat an empty tree and a real
     * subtree uniformly. Updated by insert/delete and by the rotation helpers. */
    rb_node_t *root;

    /* The single shared sentinel standing in for every NULL child and for the
     * parent of the root. Allocated once in rb_create(); its `color` is always
     * BLACK, and its key/value/child pointers are unused. Using one sentinel
     * (rather than NULL) removes the null checks from the red-black fixup and
     * lets delete-fixup temporarily write `nil->parent`. Must be freed exactly
     * once, by rb_destroy(), after all real nodes are gone. */
    rb_node_t *nil;

    /* Caller-supplied destructor for owned values, captured from rb_create().
     * May be NULL, meaning the tree does not own the values and must not free
     * them. When non-NULL it is invoked on the old value when a key is
     * overwritten by rb_insert(), on the removed value by rb_delete(), and on
     * every remaining value by rb_destroy(). */
    rb_value_free_fn value_free;

    /* Number of key/value pairs currently stored (excludes the `nil` sentinel).
     * Maintained incrementally: +1 on a successful insert of a new key, +0 when
     * an insert overwrites an existing key, -1 on a successful delete. Returned
     * as-is by rb_size(), which is therefore O(1). */
    size_t size;
};

/* Allocates an empty tree and its `nil` sentinel.
 * `value_free` is stored verbatim; NULL means the tree does not own values.
 * Returns NULL on allocation failure, having freed any partial allocation. */
rbtree_t *rb_create(rb_value_free_fn value_free) {
    rbtree_t *t = malloc(sizeof *t);
    if (t == NULL) {
        return NULL;
    }

    rb_node_t *nil = malloc(sizeof *nil);
    if (nil == NULL) {
        goto fail_tree;
    }
    /* The sentinel is always black; its links point at itself so that a stray
     * traversal off a leaf terminates instead of dereferencing NULL. */
    nil->key = NULL;
    nil->value = NULL;
    nil->color = BLACK;
    nil->parent = nil;
    nil->left = nil;
    nil->right = nil;

    t->nil = nil;
    t->root = nil; /* empty tree: root == nil, never NULL */
    t->value_free = value_free;
    t->size = 0;
    return t;

fail_tree:
    free(t);
    return NULL;
}

/* Post-order free of one subtree. Depth is O(log n) for a valid RB tree. */
static void rb_free_subtree(rbtree_t *t, rb_node_t *n) {
    if (n == t->nil) {
        return;
    }
    rb_free_subtree(t, n->left);
    rb_free_subtree(t, n->right);
    free(n->key);
    if (t->value_free != NULL) {
        t->value_free(n->value);
    }
    free(n);
}

/* Frees every node, key copy, and (if owned) value, then the tree and its
 * sentinel. NULL-safe per the contract. */
void rb_destroy(rbtree_t *t) {
    if (t == NULL) {
        return;
    }
    rb_free_subtree(t, t->root);
    free(t->nil);
    free(t);
}