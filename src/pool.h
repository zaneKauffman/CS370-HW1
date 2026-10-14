#ifndef POOL_H
#define POOL_H
#include <stddef.h>
typedef struct rb_pool rb_pool_t;
/* slabs are 4096 bytes */
rb_pool_t *pool_create(size_t obj_size);
/* O(1), from a free list */
void *pool_alloc(rb_pool_t *p);
/* O(1), returns to the free list */
void pool_free(rb_pool_t *p, void *obj);
void pool_stats(const rb_pool_t *p,
size_t *slabs, size_t *live, size_t *free_objs);
/* releases all slabs */
void pool_destroy(rb_pool_t *p);
#endif
