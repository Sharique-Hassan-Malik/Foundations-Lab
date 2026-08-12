# c-coroutines

Stackful, cooperative **coroutines** for C, built on a **hand-written x86-64 context switch**. A coroutine is a function that runs on its own stack and can suspend itself mid-execution (`co_yield`) and be continued later exactly where it left off (`co_resume`) — the primitive underneath generators, cooperative schedulers, and async-I/O runtimes. From scratch, including the twelve-instruction assembly switch and the fabricated first-run stack frame.

## The headline: a full context switch in a dozen instructions, at ~26 ns

Switching coroutines means swapping *stacks*. The System V ABI already guarantees that all the scratch registers are dead at a call boundary, so the entire context that must be saved is six callee-saved registers plus the stack pointer. `co_switch` pushes those six onto the current stack, stashes the stack pointer, loads the other coroutine's stack pointer, pops its six registers, and `ret`s into wherever that stack was suspended:

```
$ make bench

  100000000 context switches in 2.648 s
  = 37.8 million switches/second  (26.5 ns each)
```

Nearly 38 million switches a second — a coroutine switch is roughly the cost of a function call, because that is essentially all it is.

## Correctness

Suspend/resume is only useful if it preserves *everything*. The tests check exactly that, and the suite is **AddressSanitizer-clean**:

```
$ make test

generator: yields a sequence, then finishes
  ok:   resumes yield 0..9 in order, not yet done
two-way: co_resume(in) delivers a value the coroutine receives
  ok:   values pass in and out symmetrically
scheduler: two coroutines interleave in the order we resume them
nesting: a coroutine can resume another coroutine
  ok:   inner values bubble up through the outer coroutine
stack: a coroutine's locals survive switching away and back
  ok:   the 2048-element local array is byte-for-byte intact
scale: hundreds of coroutines coexist
  ok:   all 500 coroutines ran to completion

ALL TESTS PASSED
```

The load-bearing test is **stack integrity**: a coroutine fills a 2048-element local array, yields, lets 50 other coroutine switches run (clobbering registers and other stacks), then resumes and recomputes from that array — which must be byte-for-byte intact. A broken save/restore fails this immediately. Values pass both ways across a switch (like Python's `generator.send`), coroutines can resume other coroutines (nesting), and hundreds coexist.

## Using it

```c
#include "coroutine.h"

void gen(void *arg) {
    long n = (long)arg;
    for (long i = 0; i < n; i++) co_yield(i);   // suspend, hand i to the resumer
}

coroutine *co = co_create(gen, (void *)5, 0);   // 0 = default stack size
long v;
while (!co_done(co)) v = co_resume(co, 0);      // 0, 1, 2, 3, 4
co_destroy(co);
```

## How it works

Each coroutine gets its own stack, `mmap`'d with a `PROT_NONE` **guard page** at the low end so a stack overflow faults immediately instead of silently corrupting the neighbouring allocation. The subtle part is the *first* resume: a brand-new coroutine has never run, so there is no suspended stack to restore. `co_create` fabricates one — six zeroed register slots and a return address pointing at a trampoline — so that the identical pop/`ret` sequence in `co_switch` that resumes a parked coroutine instead *launches* a fresh one into `fn(arg)`.

## Build & run

```bash
make test      # build (C + assembly) and run the test suite
make bench      # context-switch throughput
make clean

# the suite is sanitiser-clean:
gcc -O1 -g -std=c11 -Iinclude -fsanitize=address src/coroutine.c src/switch.S tests/test_coroutine.c -o t && ./t
```

x86-64 Linux, a C11 compiler, and `mmap`. (The context switch is x86-64 System V assembly; the rest is portable C.)

## Layout

| path | what it holds |
|---|---|
| `src/switch.S` | the context switch: save 6 registers + rsp, swap stacks, restore, `ret` |
| `src/coroutine.c` | create/resume/yield/destroy, the guard-page stack, the first-run trampoline |
| `include/coroutine.h` | the public API |
| `tests/test_coroutine.c` | generator, two-way values, scheduling, nesting, stack integrity, scale |
| `bench/bench.c` | context-switch throughput |

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for the register set the ABI lets us skip, the exact initial-stack layout, and why the trampoline needs no arguments.

## License

MIT — see [`LICENSE`](./LICENSE).
