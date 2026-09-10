#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Number of key/value pairs stored. NULL-safe, matching rb_destroy's spirit. */
size_t rb_size(const rbtree_t *t) {
    return (t == NULL) ? 0 : t->size;
}

/* Recursively checks the subtree rooted at n. Returns that subtree's
 * black-height (black nodes on any n-to-nil path, counting the terminal nil),
 * or -1 if any red-black or BST invariant is violated within it.
 * invariant: on return *prev is the greatest key visited so far by the global
 * in-order walk (NULL before the first key), and *count has been incremented
 * once per real (non-nil) node visited so far.
 * Assumes an acyclic tree; a corrupted cycle would recurse unboundedly (out of
 * scope for this milestone). */
static int rb_check(const rbtree_t *t, const rb_node_t *n,
                    const char **prev, size_t *count) {
    if (n == t->nil) {
        return 1; /* terminal nil counts as one black node */
    }

    /* property 2: a red node must not have a red child (nil reads BLACK) */
    if (n->color == RED &&
        (n->left->color == RED || n->right->color == RED)) {
        return -1;
    }

    int lh = rb_check(t, n->left, prev, count);
    if (lh < 0) {
        return -1;
    }

    /* property 4: in-order keys strictly increasing (checked at the in-order
     * position, between the left and right recursion) */
    if (*prev != NULL && strcmp(*prev, n->key) >= 0) {
        return -1;
    }
    *prev = n->key;
    *count += 1; /* property 5: count this real node */

    int rh = rb_check(t, n->right, prev, count);
    if (rh < 0) {
        return -1;
    }

    /* property 3: both sides have equal black-height */
    if (lh != rh) {
        return -1;
    }

    return lh + (n->color == BLACK ? 1 : 0);
}

/* Returns 0 iff all red-black invariants hold, -1 otherwise. */
int rb_validate(const rbtree_t *t) {
    if (t == NULL) {
        return -1; /* a NULL tree is not a valid RB tree */
    }

    /* property 1: root is black. Empty tree: root == nil, which is BLACK, so
     * the empty tree is accepted here too. */
    if (t->root->color != BLACK) {
        return -1;
    }

    const char *prev = NULL; /* greatest key seen by the in-order walk */
    size_t count = 0;        /* real nodes seen by the walk */
    if (rb_check(t, t->root, &prev, &count) < 0) {
        return -1;
    }

    /* property 5: cached size matches the real node count */
    if (count != t->size) {
        return -1;
    }

    return 0;
}

/* Heap copy of a NUL-terminated key; the tree owns the result. NULL on OOM.
 * Not strdup(): under -std=c23 (strict) the glibc headers may not declare it,
 * and an implicit declaration would trip -Werror. */
static char *dup_key(const char *key) {
    size_t n = strlen(key) + 1;
    char *copy = malloc(n);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, key, n);
    return copy;
}

/* CLRS left rotation, sentinel form. Assumes x->right != t->nil. Retargets
 * t->root when x was the root. */
static void left_rotate(rbtree_t *t, rb_node_t *x) {
    rb_node_t *y = x->right; /* y takes x's slot; x becomes y->left */
    x->right = y->left;
    if (y->left != t->nil) {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == t->nil) {
        t->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

/* Mirror of left_rotate. Assumes x->left != t->nil. */
static void right_rotate(rbtree_t *t, rb_node_t *x) {
    rb_node_t *y = x->left; /* y takes x's slot; x becomes y->right */
    x->left = y->right;
    if (y->right != t->nil) {
        y->right->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == t->nil) {
        t->root = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }
    y->right = x;
    x->parent = y;
}

/* CLRS RB-INSERT-FIXUP. z is the freshly linked RED node; restore the red-black
 * invariants by recoloring and rotating up the tree. */
static void rb_insert_fixup(rbtree_t *t, rb_node_t *z) {
    /* invariant: z is RED; the only invariant that can be broken is a red z
     * with a red parent. When z is the root, z->parent is t->nil (BLACK) and
     * the loop exits immediately. */
    while (z->parent->color == RED) {
        if (z->parent == z->parent->parent->left) {
            rb_node_t *uncle = z->parent->parent->right;
            if (uncle->color == RED) {
                /* case 1: red uncle -> recolor and move up */
                z->parent->color = BLACK;
                uncle->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    /* case 2: triangle -> rotate to a line */
                    z = z->parent;
                    left_rotate(t, z);
                }
                /* case 3: line -> recolor and rotate the grandparent */
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                right_rotate(t, z->parent->parent);
            }
        } else {
            /* mirror image: parent is a right child */
            rb_node_t *uncle = z->parent->parent->left;
            if (uncle->color == RED) {
                z->parent->color = BLACK;
                uncle->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    right_rotate(t, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                left_rotate(t, z->parent->parent);
            }
        }
    }
    t->root->color = BLACK;
}

/* Inserts or overwrites key. On success (return 0) the tree owns value; on any
 * failure (return -1) the tree is unchanged and value is NOT consumed. */
int rb_insert(rbtree_t *t, const char *key, void *value) {
    if (t == NULL || key == NULL) {
        return -1; /* invalid args: tree unchanged, value NOT consumed */
    }

    /* phase 1: read-only BST descent, remembering the parent and last compare.
     * invariant: key, if present, is in the subtree rooted at cur; parent is
     * cur's parent. */
    rb_node_t *parent = t->nil;
    rb_node_t *cur = t->root;
    int cmp = 0;
    while (cur != t->nil) {
        cmp = strcmp(key, cur->key);
        if (cmp == 0) {
            /* overwrite existing key: no structural change, no size change.
             * The cur->value != NULL guard avoids handing a NULL to a possibly
             * non-NULL-safe value_free. */
            if (t->value_free != NULL && cur->value != NULL) {
                t->value_free(cur->value);
            }
            cur->value = value; /* ownership taken on this success path */
            return 0;
        }
        parent = cur;
        cur = (cmp < 0) ? cur->left : cur->right;
    }

    /* phase 2: allocate everything before mutating the tree */
    rb_node_t *n = malloc(sizeof *n);
    if (n == NULL) {
        goto fail;
    }
    n->key = dup_key(key);
    if (n->key == NULL) {
        goto fail_node;
    }
    n->value = value;
    n->color = RED;
    n->left = t->nil;
    n->right = t->nil;
    n->parent = parent;

    /* phase 3: link the node (no allocation past this point) */
    if (parent == t->nil) {
        t->root = n; /* first node in the tree */
    } else if (cmp < 0) { /* cmp == strcmp(key, parent->key); loop ran >= 1 */
        parent->left = n;
    } else {
        parent->right = n;
    }
    t->size++;

    /* phase 4: restore the red-black invariants */
    rb_insert_fixup(t, n);
    return 0;

fail_node:
    free(n);
fail:
    return -1; /* tree untouched, value not consumed */
}

/* Returns the value stored under key, or NULL if absent. Tree retains ownership. */
void *rb_find(const rbtree_t *t, const char *key) {
    if (t == NULL || key == NULL) {
        return NULL;
    }
    const rb_node_t *x = t->root;
    /* invariant: key, if present, lies in the subtree rooted at x */
    while (x != t->nil) {
        int c = strcmp(key, x->key);
        if (c == 0) {
            return x->value;
        }
        x = (c < 0) ? x->left : x->right;
    }
    return NULL;
}