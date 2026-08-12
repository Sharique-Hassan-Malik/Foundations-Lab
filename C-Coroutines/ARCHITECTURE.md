# Architecture

A stackful coroutine is a function with its own stack that can be paused and
continued. Everything hard about that reduces to one operation — swapping which
stack the CPU is using — plus the bookkeeping to start a stack that has never
run. Two files: the swap in assembly, the bookkeeping in C.

## What a switch actually has to save

Naïvely, switching execution contexts sounds like it means saving all the
registers. It doesn't. `co_switch` is only ever called as a *function*, and the
System V AMD64 ABI divides registers into caller-saved (the compiler already
spilled anything it cared about before the call) and callee-saved (the callee
must preserve them). So at the moment of a call, the only live state a switch
must carry across is:

- the six **callee-saved** registers: `rbx`, `rbp`, `r12`, `r13`, `r14`, `r15`;
- the **stack pointer** `rsp` (which implicitly carries the return address and
  all locals, since they live on the stack).

Everything else is already dead. That is why `co_switch` is a dozen instructions:

```asm
co_switch:                 ; rdi = save_sp (void**), rsi = restore_sp (void*)
    push rbp … push r15    ; save the 6 callee-saved regs on THIS stack
    mov  rsp, [rdi]        ; *save_sp = rsp   (park this coroutine)
    mov  rsi, rsp         ; rsp = restore_sp  (switch stacks)
    pop  r15 … pop rbp     ; restore the other coroutine's 6 regs
    ret                    ; jump to its saved return address
```

The `ret` is the pivot: it pops a return address off the *restored* stack and
jumps there. For a parked coroutine that address is the instruction right after
its own `co_switch` call — so it resumes mid-function. The C side never sees any
of this; from its point of view `co_switch` is a normal call that happens to
return "in a different coroutine."

## Starting a stack that has never run

A brand-new coroutine has no suspended state, so its first resume can't restore
anything real — yet it must go through the exact same pop/`ret` path, because
that path is the only way in. `co_create` solves this by *fabricating* a stack
that looks like a coroutine parked at the very beginning:

```
   high ┌───────────────────────┐  ← 16-byte aligned top
        │  return addr = trampoline │   ← what `ret` will jump to
        │  rbp = 0                  │ ┐
        │  rbx = 0                  │ │  the six register slots the
        │  r12 = 0                  │ │  pops will load (values irrelevant)
        │  r13 = 0                  │ │
        │  r14 = 0                  │ │
   sp → │  r15 = 0                  │ ┘
   low  └───────────────────────┘
```

`co->sp` points at the bottom of that block. On the first `co_switch` into it,
the six pops discard the zeros, and `ret` jumps to the **trampoline** instead of
into the middle of some function. The alignment is chosen so that when the
trampoline makes its first real `call`, `rsp % 16 == 8` exactly as the ABI
requires at a function entry.

The trampoline takes no arguments — it can't, because the registers were just
loaded with zeros. Instead it reads a module-global `g_current`, which
`co_resume` sets to the coroutine it is about to switch into. From there it calls
`fn(arg)`, and when `fn` returns it marks the coroutine done and switches back to
the resumer for good.

## Resume, yield, and nesting

The C bookkeeping is small. Each coroutine stores two saved stack pointers: its
own (`sp`, while parked) and its resumer's (`caller_sp`). `co_resume(co, in)`
sets the in-flight value, records the current coroutine as the one to return to,
switches in (saving the resumer into `co->caller_sp` and jumping to `co->sp`),
and — when control comes back — returns whatever the coroutine yielded.
`co_yield(out)` is the mirror: store the out value, switch back to `caller_sp`,
and return the next resume's input. A single `value` field ferries data across in
both directions.

Because `g_current` and each coroutine's `caller_sp` are updated on every switch,
coroutines can resume *other* coroutines: when an outer coroutine resumes an
inner one, the inner one's `caller_sp` points into the outer, so the inner's
yield returns to the outer, and the outer's yield returns to `main` — the tests
verify this nesting explicitly.

## The guard page

Stackful coroutines have fixed-size stacks, so overflow is a real hazard. Each
stack is `mmap`'d with one extra page at the low-address end marked `PROT_NONE`.
If a coroutine recurses too deep and the stack pointer walks into that page, the
hardware raises a fault at the moment of overflow — a clean crash pointing at the
cause — rather than letting the write silently corrupt whatever memory happened
to sit below the stack. It costs one page per coroutine and turns a
heisenbug-class failure into an immediate, obvious one.
