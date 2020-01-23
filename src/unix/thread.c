#include <unix_internal.h>
#include <ftrace.h>

thread dummy_thread;

sysreturn gettid()
{
    return current->tid;
}

sysreturn set_tid_address(int *a)
{
    current->clear_tid = a;
    return current->tid;
}

sysreturn arch_prctl(int code, unsigned long addr)
{    
    thread_log(current, "arch_prctl: code 0x%x, addr 0x%lx", code, addr);
    switch (code) {
    case ARCH_SET_GS:
        current->frame[FRAME_GSBASE] = addr;
        break;
    case ARCH_SET_FS:
        current->frame[FRAME_FSBASE] = addr;
        return 0;
    case ARCH_GET_FS:
	if (!addr)
            return set_syscall_error(current, EINVAL);
	*(u64 *) addr = current->frame[FRAME_FSBASE];
        break;
    case ARCH_GET_GS:
	if (!addr)
            return set_syscall_error(current, EINVAL);
	*(u64 *) addr = current->frame[FRAME_GSBASE];
        break;
    default:
        return set_syscall_error(current, EINVAL);
    }
    return 0;
}

sysreturn clone(unsigned long flags, void *child_stack, int *ptid, int *ctid, unsigned long newtls)
{
    thread_log(current, "clone: flags %lx, child_stack %p, ptid %p, ctid %p, newtls %lx",
        flags, child_stack, ptid, ctid, newtls);

    if (!child_stack)   /* this is actually a fork() */
    {
        thread_log(current, "attempted to fork by passing "
                   "null child stack, aborting.");
        return set_syscall_error(current, ENOSYS);
    }

    /* clone thread context up to FRAME_VECTOR */
    thread t = create_thread(current->p);
    runtime_memcpy(t->frame, current->frame, sizeof(u64) * FRAME_ERROR_CODE);
    thread_clone_sigmask(t, current);

    /* clone behaves like fork at the syscall level, returning 0 to the child */
    set_syscall_return(t, 0);
    t->frame[FRAME_RSP]= u64_from_pointer(child_stack);
    t->frame[FRAME_FSBASE] = newtls;
    if (flags & CLONE_PARENT_SETTID)
        *ptid = t->tid;
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_tid = ctid;
    schedule_frame(&t->frame);
    return t->tid;
}


void register_thread_syscalls(struct syscall *map)
{
    register_syscall(map, futex, futex);
    register_syscall(map, clone, clone);
    register_syscall(map, arch_prctl, arch_prctl);
    register_syscall(map, set_tid_address, set_tid_address);
    register_syscall(map, gettid, gettid);
}

void thread_log_internal(thread t, const char *desc, ...)
{
    //    if (table_find(t->p->process_root, sym(trace))) {
        if (syscall_notrace(t->syscall))
            return;
        vlist ap;
        vstart (ap, desc);        
        buffer b = allocate_buffer(transient, 100);
        bprintf(b, "%n%d ", (int) ((MAX(MIN(t->tid, 20), 1) - 1) * 4), t->tid);
        if (current->name[0] != '\0')
            bprintf(b, "[%s] ", current->name);
        buffer f = alloca_wrap_buffer(desc, runtime_strlen(desc));
        vbprintf(b, f, &ap);
        push_u8(b, '\n');
        buffer_print(b);
        //    }
}

// this is a parallel construction with IRETURN and frame_return..fix
extern void *interrupt_exit_rbx();
static void fix_me_iret_wrapper(context f)
{
    // we should change this to take rdi .. the asm doc is pretty explicit
    // about not really excluding rbx from other allocations
    register u64 rbx __asm__("%rbx") = u64_from_pointer(f);
    __asm__("jmp interrupt_rbx_return"::"r"(rbx));
}

// this is a parallel construction with IRETURN and frame_return..fix
extern void *frame_return();
static void fix_me_sysret_wrapper(context f)
{
    // we should change this to take rdi .. the asm doc is pretty explicit
    // about not really excluding rbx from other allocations
    register u64 rbx __asm__("%rbx") = u64_from_pointer(f);
    __asm__("jmp frame_return"::"r"(rbx));
}

#define TRAP_FLAG 0x100

typedef void (*restorer)(context);
closure_function(2, 0, void, run_thread,
                 thread, t,
                 restorer, restore_function)
{
    thread t = bound(t);
    thread old = current;
    current_cpu()->current_thread = t;
    rprintf("run thread\n");
    /* ftrace needs to know about the switch event */
    ftrace_thread_switch(old, current);

    thread_log(t, "run frame %p, RIP=%p", t->frame, t->frame[FRAME_RIP]);
    proc_enter_user(current->p);
    set_running_frame(t->frame);

    /* cover wake-before-sleep situations (e.g. sched yield, fs ops that don't go to disk, etc.) */
    t->blocked_on = 0;
    t->syscall = -1;

    /* check if we have a pending signal */
    dispatch_signals(t);

    context f = current_cpu()->running_frame;
    /* running frame may have changed to signal handling frame */
    f[FRAME_FLAGS] |= U64_FROM_BIT(FLAG_INTERRUPT);
    f[FRAME_FLAGS] |= TRAP_FLAG;    
    rprintf("run thread %p %p %p\n", t, f, f[FRAME_RIP]);
    bound(restore_function)(f);
}

void thread_sleep_interruptible(void)
{
    disable_interrupts();
    assert(current->blocked_on);
    thread_log(current, "sleep interruptible (on \"%s\")", blockq_name(current->blocked_on));
    runloop();
}

void thread_sleep_uninterruptible(void)
{
    disable_interrupts();
    assert(!current->blocked_on);
    current->blocked_on = INVALID_ADDRESS;
    thread_log(current, "sleep uninterruptible");
    runloop();
}

void thread_yield(void)
{
    disable_interrupts();
    thread_log(current, "yield %d, RIP=0x%lx", current->tid, current->frame[FRAME_RIP]);
    assert(!current->blocked_on);
    current->syscall = -1;
    set_syscall_return(current, 0);
    schedule_frame(current->frame);
    runloop();
}

void thread_wakeup(thread t)
{
    thread_log(current, "%s: %ld->%ld blocked_on %s, RIP=0x%lx", __func__, current->tid, t->tid,
               t->blocked_on ? (t->blocked_on != INVALID_ADDRESS ? blockq_name(t->blocked_on) : "uninterruptible") :
               "(null)", t->frame[FRAME_RIP]);
    assert(t->blocked_on);
    schedule_frame((context)t);
}

boolean thread_attempt_interrupt(thread t)
{
    thread_log(current, "%s: tid %d", __func__, t->tid);
    if (!thread_in_interruptible_sleep(t)) {
        thread_log(current, "uninterruptible or already running");
        return false;
    }

    /* flush pending blockq */
    thread_log(current, "... interrupting blocked thread %d", t->tid);
    assert(blockq_flush_thread(t->blocked_on, t));
    assert(thread_is_runnable(t));
    return true;
}

define_closure_function(1, 0, void, free_thread,
                        thread, t)
{
    deallocate(heap_general(get_kernel_heaps()), bound(t), sizeof(struct thread));
}

thread create_thread(process p)
{
    // heap I guess
    static int tidcount = 0;
    heap h = heap_general((kernel_heaps)p->uh);

    thread t = allocate(h, sizeof(struct thread));
    if (t == INVALID_ADDRESS)
        goto fail;

    t->thread_bq = allocate_blockq(h, "thread");
    if (t->thread_bq == INVALID_ADDRESS)
        goto fail_bq;

    t->signalfds = allocate_notify_set(h);
    if (t->signalfds == INVALID_ADDRESS)
        goto fail_sfds;

    t->p = p;
    t->syscall = -1;
    t->uh = *p->uh;
    init_refcount(&t->refcount, 1, init_closure(&t->free, free_thread, t));
    t->select_epoll = 0;
    t->tid = tidcount++;
    t->clear_tid = 0;
    t->name[0] = '\0';
    zero(t->frame, sizeof(t->frame));
    
    t->frame[FRAME_FAULT_HANDLER] = u64_from_pointer(create_fault_handler(h, t));
    // xxx - dup with allocate_frame
    t->frame[FRAME_QUEUE] = u64_from_pointer(thread_queue);
    t->frame[FRAME_IRET] = u64_from_pointer(closure(h, run_thread, t, fix_me_iret_wrapper));
    t->frame[FRAME_SYSRETURN] = u64_from_pointer(closure(h, run_thread, t, fix_me_sysret_wrapper));
    t->frame[FRAME_RUN] = t->frame[FRAME_SYSRETURN];
    
    t->blocked_on = 0;
    t->file_op_is_complete = false;
    init_sigstate(&t->signals);
    t->dispatch_sigstate = 0;
    t->active_signo = 0;

    if (ftrace_thread_init(t)) {
        msg_err("failed to init ftrace state for thread\n");
        deallocate_blockq(t->thread_bq);
        deallocate(h, t, sizeof(struct thread));
        return INVALID_ADDRESS;
    }

    // XXX sigframe
    vector_set(p->threads, t->tid, t);
    return t;
  fail_sfds:
    deallocate_blockq(t->thread_bq);
  fail_bq:
    deallocate(h, t, sizeof(struct thread));
  fail:
    msg_err("%s: failed to allocate\n", __func__);
    return INVALID_ADDRESS;
}

NOTRACE 
void exit_thread(thread t)
{
    thread_log(current, "exit_thread");

    assert(vector_length(t->p->threads) > t->tid);
    vector_set(t->p->threads, t->tid, 0);

    /* We might be exiting from the signal handler while dispatching a
       signal on behalf of the process sigstate, so reset masks as if
       we're returning from the signal handler. */
    sigstate_thread_restore(t);

    /* dequeue signals for thread */
    sigstate_flush_queue(&t->signals);

    /* We don't yet support forcible removal while in an uninterruptible wait. */
    assert(t->blocked_on != INVALID_ADDRESS);

    /* Kill received during interruptible wait. */
    if (t->blocked_on)
        blockq_flush_thread(t->blocked_on, t);

    if (t->clear_tid) {
        *t->clear_tid = 0;
        futex_wake_one_by_uaddr(t->p, t->clear_tid); /* ignore errors */
    }

    if (t->select_epoll)
        epoll_finish(t->select_epoll);

    /* XXX futex robust list needs implementing - wake up robust futexes here */

    blockq_flush(t->thread_bq);
    deallocate_blockq(t->thread_bq);
    t->thread_bq = INVALID_ADDRESS;

    deallocate_closure(t->run); // structure closure
    t->run = INVALID_ADDRESS;

    deallocate_closure((fault_handler)pointer_from_u64(t->frame[FRAME_FAULT_HANDLER]));
    t->frame[FRAME_FAULT_HANDLER] = 0;

    ftrace_thread_deinit(t, dummy_thread);

    // not sure if serious - maybe ftrace needs this?
    //    current_cpu()->current_thread = dummy_thread;
    //    set_running_frame(dummy_thread->frame);
    refcount_release(&t->refcount);
}

void init_threads(process p)
{
    heap h = heap_general((kernel_heaps)p->uh);
    p->threads = allocate_vector(h, 5);
    init_futices(p);
}
