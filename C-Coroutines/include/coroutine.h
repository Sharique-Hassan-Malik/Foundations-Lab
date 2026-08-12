/* coroutine — stackful, cooperative coroutines for C, built on a hand-written
 * x86-64 context switch.
 *
 * A coroutine is a function running on its own stack that can suspend itself in
 * the middle (`co_yield`) and be continued later (`co_resume`) exactly where it
 * left off — the basis of generators, cooperative schedulers, and async I/O
 * runtimes. Unlike a callback, it keeps its local variables and its place in the
 * call stack across a suspension.
 *
 * A value is handed across each switch in both directions (like Python's
 * generator send/yield): `co_yield(out)` passes `out` to the resumer and returns
 * the `in` value the next `co_resume` supplies; `co_resume(co, in)` passes `in`
 * to the coroutine and returns the `out` it yields. */
#ifndef COROUTINE_H
#define COROUTINE_H

#include <stddef.h>

typedef struct coroutine coroutine;

/* Create a coroutine that will run `fn(arg)` on a fresh `stack_size`-byte stack
 * (0 = a sensible default). It does not start until the first co_resume. */
coroutine *co_create(void (*fn)(void *), void *arg, size_t stack_size);

/* Resume `co`, passing it `in`, running it until it yields or returns. Returns
 * the value the coroutine passed to co_yield (or 0 when it returns / is already
 * finished). */
long co_resume(coroutine *co, long in);

/* Suspend the current coroutine, handing `out` back to whoever resumed it.
 * Returns the `in` value supplied by the next co_resume. Must be called from
 * inside a coroutine. */
long co_yield(long out);

/* True once the coroutine's function has returned. */
int  co_done(const coroutine *co);

/* Free a coroutine and its stack. */
void co_destroy(coroutine *co);

#endif /* COROUTINE_H */
