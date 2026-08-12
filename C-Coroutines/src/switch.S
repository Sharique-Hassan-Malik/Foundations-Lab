/* co_switch(void **save_sp, void *restore_sp)  — x86-64 System V.
 *
 * A cooperative context switch in a dozen instructions. The System V ABI says a
 * function must preserve rbx, rbp, and r12-r15 across calls, so those six
 * registers are the entire "context" that a switch has to save — everything else
 * is already dead at a call boundary. We push them onto the *current* stack,
 * record the resulting stack pointer through `save_sp` (rdi), load the other
 * coroutine's stack pointer from `restore_sp` (rsi), pop its six saved registers,
 * and `ret` — which jumps to wherever that stack's return address points.
 *
 * The magic is that the return address on the restored stack is either the point
 * another coroutine last switched away from, or (on the very first switch) the
 * trampoline that starts the coroutine — see co_create in coroutine.c. */

    .text
    .globl co_switch
    .type  co_switch, @function
    .align 16
co_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, (%rdi)      /* *save_sp = rsp  (this coroutine is now parked) */
    movq  %rsi, %rsp        /* rsp = restore_sp  (switch to the other stack)  */
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret
    .size co_switch, .-co_switch

/* Mark the stack non-executable (no GNU stack note otherwise → linker warning). */
    .section .note.GNU-stack,"",@progbits
