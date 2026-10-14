#include <stdalign.h>
#include <stddef.h>
#include "pool.h"
#include "fault_alloc.h"

#define SLAB_BYTES 4096

/* A freed object's first bytes hold the link to the next free object. */
typedef struct free_obj {
    struct free_obj *next;
} free_obj_t;

/* Header at the start of every slab. The union pads it to max_align_t so the
 * first object carved after it is suitably aligned for any type. */
typedef union slab {
    union slab *next; /* singly linked list of all slabs, newest first */
    max_align_t align;
} slab_t;

struct rb_pool {
    size_t obj_size;     /* requested size rounded up for alignment and link */
    size_t per_slab;     /* objects carved from one slab */
    slab_t *slabs;       /* every slab owned by the pool, newest first */
    free_obj_t *free;    /* objects returned by pool_free, LIFO */
    char *bump;          /* next never-used object in the newest slab */
    char *bump_end;      /* end of the newest slab's object area */
    size_t nslabs;       /* length of the slabs list */
    size_t live;         /* objects handed out and not yet freed */
};

/* Returns NULL if obj_size is 0, too big to fit one object in a slab, or on
 * allocation failure. Slabs are allocated lazily by pool_alloc. */
rb_pool_t *pool_create(size_t obj_size) {
    const size_t align = alignof(max_align_t);
    const size_t room = SLAB_BYTES - sizeof(slab_t);
    if (obj_size == 0 || obj_size > room) {
        return NULL;
    }
    if (obj_size < sizeof(free_obj_t)) {
        obj_size = sizeof(free_obj_t);
    }
    obj_size = (obj_size + align - 1) / align * align;
    if (obj_size > room) {
        return NULL;
    }

    rb_pool_t *p = rb_malloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    p->obj_size = obj_size;
    p->per_slab = room / obj_size;
    p->slabs = NULL;
    p->free = NULL;
    p->bump = NULL;
    p->bump_end = NULL;
    p->nslabs = 0;
    p->live = 0;
    return p;
}

/* Free list first, then the newest slab's untouched tail, then a new slab.
 * O(1) apart from rb_malloc. NULL on allocation failure, pool unchanged. */
void *pool_alloc(rb_pool_t *p) {
    if (p == NULL) {
        return NULL;
    }
    if (p->free != NULL) {
        free_obj_t *o = p->free;
        p->free = o->next;
        p->live++;
        return o;
    }
    if (p->bump == p->bump_end) {
        slab_t *s = rb_malloc(SLAB_BYTES);
        if (s == NULL) {
            return NULL;
        }
        s->next = p->slabs;
        p->slabs = s;
        p->nslabs++;
        p->bump = (char *)(s + 1);
        p->bump_end = p->bump + p->per_slab * p->obj_size;
    }
    void *o = p->bump;
    p->bump += p->obj_size;
    p->live++;
    return o;
}

/* obj must have come from pool_alloc(p) and not already be freed. NULL-safe. */
void pool_free(rb_pool_t *p, void *obj) {
    if (p == NULL || obj == NULL) {
        return;
    }
    free_obj_t *o = obj;
    o->next = p->free;
    p->free = o;
    p->live--;
}

/* Any out-pointer may be NULL. free_objs counts both freed objects and the
 * never-used tail of the newest slab. */
void pool_stats(const rb_pool_t *p,
                size_t *slabs, size_t *live, size_t *free_objs) {
    size_t ns = (p == NULL) ? 0 : p->nslabs;
    size_t nl = (p == NULL) ? 0 : p->live;
    size_t cap = (p == NULL) ? 0 : ns * p->per_slab;
    if (slabs != NULL) {
        *slabs = ns;
    }
    if (live != NULL) {
        *live = nl;
    }
    if (free_objs != NULL) {
        *free_objs = cap - nl;
    }
}

/* Releases every slab and the pool itself; outstanding objects die with it.
 * NULL-safe. */
void pool_destroy(rb_pool_t *p) {
    if (p == NULL) {
        return;
    }
    slab_t *s = p->slabs;
    /* invariant: s heads the list of slabs not yet freed */
    while (s != NULL) {
        slab_t *next = s->next;
        rb_free(s);
        s = next;
    }
    rb_free(p);
}
