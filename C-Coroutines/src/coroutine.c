/* Coroutine bookkeeping around the assembly context switch (switch.S).
 *
 * Each coroutine owns a stack, mmap'd with a PROT_NONE guard page at the low end
 * so that a stack overflow faults immediately instead of silently corrupting the
 * next allocation. The tricky part is the *first* resume: the coroutine has never
 * run, so its saved stack pointer is a stack we fabricate by hand — six zeroed
 * register slots and a return address pointing at a trampoline — so that the very
 * same pop/ret sequence in co_switch that resumes a parked coroutine instead
 * launches a fresh one. */

#include "coroutine.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/* Implemented in switch.S. */
extern void co_switch(void **save_sp, void *restore_sp);

#define DEFAULT_STACK (64 * 1024)

struct coroutine {
    void  *sp;              /* saved stack pointer while parked */
    void  *caller_sp;       /* the resumer's saved stack pointer */
    void  *stack_base;      /* mmap base (guard page + stack) */
    size_t map_size;
    void (*fn)(void *);
    void  *arg;
    long   value;           /* the value in flight across a switch */
    int    done;
    int    started;
};

/* The coroutine currently running (NULL when the main context is running). */
static coroutine *g_current = NULL;

/* First code that runs inside a new coroutine. It receives no arguments (the
 * registers are zeroed), so it recovers itself from g_current, which co_resume
 * set just before switching in. */
static void co_trampoline(void) {
    coroutine *self = g_current;
    self->fn(self->arg);
    self->done = 1;
    self->value = 0;                           /* a completing resume returns 0 */
    /* Return to the resumer and never come back. */
    co_switch(&self->sp, self->caller_sp);
    __builtin_unreachable();
}

coroutine *co_create(void (*fn)(void *), void *arg, size_t stack_size) {
    if (stack_size == 0) stack_size = DEFAULT_STACK;
    long page = sysconf(_SC_PAGESIZE);
    stack_size = (stack_size + page - 1) & ~(size_t)(page - 1);
    size_t map_size = stack_size + page;      /* + one guard page */

    void *base = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;
    mprotect(base, page, PROT_NONE);          /* guard page at the low end */

    coroutine *co = calloc(1, sizeof *co);
    if (!co) { munmap(base, map_size); return NULL; }
    co->stack_base = base;
    co->map_size = map_size;
    co->fn = fn;
    co->arg = arg;

    /* Build the initial stack: [six zeroed regs][return address = trampoline],
     * 16-byte aligned so that the trampoline's own `call` is ABI-correct. */
    uintptr_t top = (uintptr_t)base + map_size;
    top &= ~(uintptr_t)15;                     /* 16-byte align the top */
    void **sp = (void **)top;
    *--sp = (void *)co_trampoline;             /* ret target */
    for (int i = 0; i < 6; i++) *--sp = 0;     /* rbp, rbx, r12-r15 */
    co->sp = sp;
    return co;
}

long co_resume(coroutine *co, long in) {
    if (co->done) return 0;
    co->value = in;                            /* delivered to the coroutine's co_yield */
    coroutine *prev = g_current;
    g_current = co;
    co->started = 1;
    /* Save our (the resumer's) context into co->caller_sp and jump to co->sp. */
    co_switch(&co->caller_sp, co->sp);
    g_current = prev;
    return co->value;                          /* the value it yielded (or 0 if it finished) */
}

long co_yield(long value) {
    coroutine *co = g_current;
    co->value = value;
    /* Park: save our context into co->sp, jump back to the resumer. */
    co_switch(&co->sp, co->caller_sp);
    return co->value;                          /* set by the next co_resume */
}

int co_done(const coroutine *co) { return co->done; }

void co_destroy(coroutine *co) {
    if (!co) return;
    munmap(co->stack_base, co->map_size);
    free(co);
}
