# rbtree-lab: project rules
## Commands
- Build & unit tests: ‘make test‘
- Sanitizers: ‘make asan‘ Valgrind: ‘make memcheck‘
- A change is DONE only when all three pass. Always run them; show output.
## Hard constraints
- NEVER modify include/rbtree.h. It is the graded contract.
- Check every allocation. malloc can return NULL; a NULL return must
leave the tree unchanged and return the documented error code.
- NEVER weaken, skip, or delete a test to make the suite pass. If a test
looks wrong, stop and explain why instead.
## Style
- C23. -Wall -Wextra -Werror must stay clean. No VLAs.
- Error handling: goto-cleanup pattern for multi-allocation functions.
- Prefer the smallest diff that passes. Do not refactor unrelated code.
- Every non-obvious loop gets a one-line invariant comment.
## Workflow
- For any multi-file or algorithmic change: propose a plan and wait for
approval before editing.
- Commit only from a green state; message format "M<n>: <what>".