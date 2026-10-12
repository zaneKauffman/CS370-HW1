#include <stddef.h>
/* Allocation wrappers. In production these forward to malloc/free;
* the test build swaps in the fault injector below. ALL allocation
* in src/rbtree.c (and src/pool.c) must go through these. */
void *rb_malloc(size_t n);
void rb_free(void *p);
/* the n-th allocation from now returns NULL */
void fault_alloc_arm(long n);
void fault_alloc_disarm(void);
/* allocations observed so far */
long fault_alloc_total(void);