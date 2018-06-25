/*
 * Read-Copy Update mechanism for mutual exclusion (PRCU version).
 * This PRCU implementation is based on a fast consensus protocol
 * published in the following paper:
 *
 * Fast Consensus Using Bounded Staleness for Scalable Read-mostly Synchronization.
 * Haibo Chen, Heng Zhang, Ran Liu, Binyu Zang, and Haibing Guan.
 * IEEE Transactions on Parallel and Distributed Systems (TPDS), 2016.
 * https://dl.acm.org/citation.cfm?id=3024114.3024143
 *
 * Authors: Heng Zhang <heng.z@huawei.com>
 *          Lihao Liang <lihao.liang@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/smp.h>
#include <linux/percpu.h>
#include <linux/prcu.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/barrier.h>

#include "rcu.h"

/* Data structures. */

/*
 * Initialize PRCU's per-CPU local structure.
 */
DEFINE_PER_CPU_SHARED_ALIGNED(struct prcu_local_struct, prcu_local);

/*
 * Initialize PRCU's global structure.
 */
struct prcu_struct global_prcu = {
       .global_version = ATOMIC64_INIT(0),
       .cb_version = ATOMIC64_INIT(0),
       .active_ctr = ATOMIC_INIT(0),
       .mtx = __MUTEX_INITIALIZER(global_prcu.mtx),
       .barrier_mtx = __MUTEX_INITIALIZER(global_prcu.barrier_mtx),
       .wait_q = __WAIT_QUEUE_HEAD_INITIALIZER(global_prcu.wait_q)
};
struct prcu_struct *prcu = &global_prcu;

/*
 * Initialize simple PRCU callback list.
 */
static void prcu_cblist_init(struct prcu_cblist *rclp)
{
       rclp->head = NULL;
       rclp->tail = &rclp->head;
       rclp->version_head = NULL;
       rclp->version_tail = &rclp->version_head;
       rclp->len = 0;
}

/*
 * Dequeue the oldest rcu_head structure from the specified callback list.
 * Store the callback version number into the version pointer.
 */
static struct rcu_head *prcu_cblist_dequeue(struct prcu_cblist *rclp)
{
       struct rcu_head *rhp;
       struct prcu_version_head *vhp;

       rhp = rclp->head;
       if (!rhp) {
               WARN_ON(vhp);
               WARN_ON(rclp->len);
               return NULL;
       }

       vhp = rclp->version_head;
       rclp->version_head = vhp->next;
       rclp->head = rhp->next;
       rclp->len--;

       if (!rclp->head) {
               rclp->tail = &rclp->head;
               rclp->version_tail = &rclp->version_head;
       }

       return rhp;
}

/* PRCU function implementations. */

/*
 * Update local PRCU state of the current CPU.
 */
static inline void prcu_report(struct prcu_local_struct *local)
{
       unsigned long long global_version;
       unsigned long long local_version;

       global_version = atomic64_read(&prcu->global_version);
       local_version = local->version;
       if (global_version > local_version)
               cmpxchg(&local->version, local_version, global_version);
}

/*
 * Mark the beginning of a PRCU read-side critical section.
 *
 * A PRCU quiescent state of a CPU is when its local ->locked and
 * ->online variables become 0.
 *
 * See prcu_read_unlock() and synchronize_prcu() for more information.
 * Also see rcu_read_lock() comment header.
 */
void prcu_read_lock(void)
{
       struct prcu_local_struct *local;

       local = get_cpu_ptr(&prcu_local);
       if (!local->online) {
               WRITE_ONCE(local->online, 1);
               /*
                * Memory barrier is needed for PRCU writers
                * to see the updated local->online value.
                */
               smp_mb();
       }
       local->locked++;
       /*
        * Critical section after entry code.
        * put_cpu_ptr() provides the needed barrier().
        */
       put_cpu_ptr(&prcu_local);
}
EXPORT_SYMBOL(prcu_read_lock);

/*
 * Mark the end of a PRCU read-side critical section.
 *
 * See prcu_read_lock() and synchronize_prcu() for more information.
 * Also see rcu_read_unlock() comment header.
 */
void prcu_read_unlock(void)
{
       int locked;
       struct prcu_local_struct *local;

       barrier(); /* Critical section before exit code. */
       local = get_cpu_ptr(&prcu_local);
       locked = local->locked;
       if (locked) {
               local->locked--;
               /*
                * If we are executing the last PRCU task,
                * update the CPU-local PRCU state.
                */
               if (locked == 1)
                       prcu_report(local);
               put_cpu_ptr(&prcu_local);
       } else {
               put_cpu_ptr(&prcu_local);
               /*
                * If we are executing the last outstanding
                * PRCU task, wake up synchronize_prcu().
                */
               if (!atomic_dec_return(&prcu->active_ctr))
                       wake_up(&prcu->wait_q);
       }
}
EXPORT_SYMBOL(prcu_read_unlock);

static void prcu_handler(void *info)
{
       struct prcu_local_struct *local;

       local = this_cpu_ptr(&prcu_local);
       /*
        * We need to do this check locally on the current CPU
        * because no memory barrier is used for ->locked so
        * PRCU writers may not see its latest local value.
        */
       if (!local->locked)
               WRITE_ONCE(local->version, atomic64_read(&prcu->global_version));
}

/*
 * Wait until a grace period has completed.
 *
 * A PRCU grace period can end if each CPU has passed a PRCU quiescent state
 * -and- the global variable ->active_ctr is 0, that is all pre-existing
 * PRCU read-side critical sections have completed.
 *
 * See prcu_read_lock() and prcu_read_unlock() for more information.
 * Also see synchronize_rcu() comment header.
 */
void synchronize_prcu(void)
{
       int cpu;
       cpumask_t cpus;
       unsigned long long version;
       struct prcu_local_struct *local;

       /*
        * Get the new global grace-period version before taking mutex,
        * which allows multiple synchronize_prcu() calls spreading PRCU
        * readers can return in a timely fashion.
        */
       version = atomic64_add_return(1, &prcu->global_version);
       /* Take mutex to serialize concurrent synchronize_prcu() calls. */
       mutex_lock(&prcu->mtx);

       local = get_cpu_ptr(&prcu_local);
       local->version = version;
       put_cpu_ptr(&prcu_local);

       cpumask_clear(&cpus);
       /* Send an IPI to force straggling CPUs to update their PRCU state. */
       for_each_possible_cpu(cpu) {
               local = per_cpu_ptr(&prcu_local, cpu);
               /*
                * If no PRCU tasks are currently running on this CPU
                * or a context-switch has occurred, the CPU-local PRCU
                * state has already been updated.
                */
               if (!READ_ONCE(local->online))
                       continue;
               if (READ_ONCE(local->version) < version) {
                       smp_call_function_single(cpu, prcu_handler, NULL, 0);
                       cpumask_set_cpu(cpu, &cpus);
               }
       }

       /* Wait for outstanding CPUs to commit. */
       for_each_cpu(cpu, &cpus) {
               local = per_cpu_ptr(&prcu_local, cpu);
               while (READ_ONCE(local->version) < version)
                       cpu_relax();
       }

       /* Wait for outstanding PRCU tasks to finish. */
       if (atomic_read(&prcu->active_ctr))
               wait_event(prcu->wait_q, !atomic_read(&prcu->active_ctr));
       /* Update the global callback version to its grace-period version. */
       atomic64_set(&prcu->cb_version, version);
       mutex_unlock(&prcu->mtx);
}
EXPORT_SYMBOL(synchronize_prcu);

/*
 * Update PRCU state when a context-switch occurs.
 */
void prcu_note_context_switch(void)
{
       struct prcu_local_struct *local;

       local = get_cpu_ptr(&prcu_local);
       /* Update local and global outstanding PRCU task number. */
       if (local->locked) {
               atomic_add(local->locked, &prcu->active_ctr);
               local->locked = 0;
       }
       /* Indicate a context-switch has occurred on this CPU. */
       local->online = 0;
       /* Update this CPU's local PRCU state. */
       prcu_report(local);
       put_cpu_ptr(&prcu_local);
}

/*
 * Queue a PRCU callback to the current CPU for invocation
 * after a grace period.
 */
void call_prcu(struct rcu_head *head, rcu_callback_t func)
{
       unsigned long flags;
       struct prcu_local_struct *local;
       struct prcu_cblist *rclp;
       struct prcu_version_head *vhp;

       debug_rcu_head_queue(head);

       /* Use GFP_ATOMIC with IRQs disabled. */
       vhp = kmalloc(sizeof(struct prcu_version_head), GFP_ATOMIC);
       /*
        * Complain about kmalloc() failure.  This could be handled
        * in a different way, e.g. return -1 to inform the caller.
        */
       if (!vhp) {
               WARN_ON(1);
               return;
       }

       head->func = func;
       head->next = NULL;
       vhp->next = NULL;

       /* Disable IRQs to prevent races with prcu_process_callbacks(). */
       local_irq_save(flags);
       local = this_cpu_ptr(&prcu_local);
       /*
        * Assign the CPU-local callback version to the given callback
        * and add it to the PRCU callback list of the current CPU.
        */
       vhp->version = local->version;
       rclp = &local->cblist;
       rclp->len++;
       *rclp->tail = head;
       rclp->tail = &head->next;
       *rclp->version_tail = vhp;
       rclp->version_tail = &vhp->next;
       local_irq_restore(flags);
}
EXPORT_SYMBOL(call_prcu);

/*
 * Check to see if there is any immediate PRCU-related work
 * to be done by the current CPU, returning 1 if so.
 *
 * Currently, it only checks whether this CPU has callbacks
 * that are ready to invoke.
 */
int prcu_pending(void)
{
       struct prcu_local_struct *local = get_cpu_ptr(&prcu_local);
       unsigned long long cb_version = local->cb_version;
       struct prcu_cblist *rclp = &local->cblist;

       put_cpu_ptr(&prcu_local);
       return cb_version < atomic64_read(&prcu->cb_version) && rclp->head;
}

/*
 * Perform PRCU core processing for the current CPU using softirq.
 */
void invoke_prcu_core(void)
{
       if (cpu_online(smp_processor_id()))
               raise_softirq(PRCU_SOFTIRQ);
}

/*
 * Schedule PRCU core processing.
 *
 * This function must be called from hardirq context.
 * It is normally invoked from the scheduling-clock interrupt.
 */
void prcu_check_callbacks(void)
{
       if (prcu_pending())
               invoke_prcu_core();
}

/*
 * Process PRCU callbacks whose grace period has completed.
 * Do this using softirq for each CPU.
 *
 * Also see the prcu_barrier() comment header.
 */
static __latent_entropy void prcu_process_callbacks(struct softirq_action *unused)
{
       unsigned long flags;
       unsigned long long cb_version;
       struct prcu_local_struct *local;
       struct prcu_cblist *rclp;
       struct rcu_head *rhp;
       struct prcu_version_head *vhp;

       if (cpu_is_offline(smp_processor_id()))
               return;

       cb_version = atomic64_read(&prcu->cb_version);

       /* Disable IRQs to prevent races with call_prcu(). */
       local_irq_save(flags);
       local = this_cpu_ptr(&prcu_local);
       rclp = &local->cblist;
       rhp = rclp->head;
       vhp = rclp->version_head;
       /*
        * Process PRCU callbacks with version number smaller
        * than the global PRCU callback version whose associated
        * grace periods have completed.
        */
       for (; rhp && vhp && vhp->version < cb_version;
            rhp = rclp->head, vhp = rclp->version_head) {
               rhp = prcu_cblist_dequeue(rclp);
               debug_rcu_head_unqueue(rhp);
               rhp->func(rhp);
       }
       /* Record the version number of callbacks to be processed. */
       local->cb_version = cb_version;
       local_irq_restore(flags);
}

/*
 * PRCU callback function for prcu_barrier().
 * If we are last, wake up the task executing prcu_barrier().
 */
static void prcu_barrier_callback(struct rcu_head *rhp)
{
       if (atomic_dec_and_test(&prcu->barrier_cpu_count))
               complete(&prcu->barrier_completion);
}

/*
 * Called with preemption disabled, and from cross-cpu IRQ context.
 */
static void prcu_barrier_func(void *info)
{
       struct prcu_local_struct *local = this_cpu_ptr(&prcu_local);

       atomic_inc(&prcu->barrier_cpu_count);
       call_prcu(&local->barrier_head, prcu_barrier_callback);
}

/*
 * Waiting for all PRCU callbacks to complete.
 *
 * NOTE: The current PRCU implementation relies on synchronize_prcu()
 * to update its global grace-period and callback version numbers.
 * If there is no synchronize_prcu() running and call_prcu() is called,
 * rcu_process_callbacks() wont't make progress and prcu_barrier() will
 * -not- return.
 *
 * This needs to be fixed, e.g. using a grace-period expediting mechanism
 * as found in the Linux-kernel RCU implementation.
 */
void prcu_barrier(void)
{
       int cpu;

       /* Take mutex to serialize concurrent prcu_barrier() requests. */
       mutex_lock(&prcu->barrier_mtx);

       /*
        * Initialize the count to one rather than to zero in order to
        * avoid a too-soon return to zero in case of a short grace period
        * (or preemption of this task).
        */
       init_completion(&prcu->barrier_completion);
       atomic_set(&prcu->barrier_cpu_count, 1);

       /*
        * Register a new callback on each CPU using IPI to prevent races
        * with call_prcu().  When that callback is invoked, we will know
        * that all of the corresponding CPU's preceding callbacks have
        * been invoked. Note that we must use the wait version of
        * smp_call_function_single().  Otherwise prcu_barrier_func()
        * might not finish incrementing prcu->barrier_cpu_count and
        * registering prcu_barrier_callback() on -each- CPU before
        * we exit the loop and wait for completion. Hence a bug!
        */
       for_each_possible_cpu(cpu)
               smp_call_function_single(cpu, prcu_barrier_func, NULL, 1);

       /* Decrement the count as we initialize it to one. */
       if (atomic_dec_and_test(&prcu->barrier_cpu_count))
               complete(&prcu->barrier_completion);

       /*
        * Now that we have an prcu_barrier_callback() callback on each
        * CPU, and thus each counted, remove the initial count.
        * Wait for all prcu_barrier_callback() callbacks to be invoked.
        */
       wait_for_completion(&prcu->barrier_completion);

       /* Other rcu_barrier() invocations can now safely proceed. */
       mutex_unlock(&prcu->barrier_mtx);
}
EXPORT_SYMBOL(prcu_barrier);

/*
 * Helper function for prcu_init() to initialize PRCU's CPU-local structure.
 */
void prcu_init_local_struct(int cpu)
{
       struct prcu_local_struct *local;

       local = per_cpu_ptr(&prcu_local, cpu);
       local->locked = 0;
       local->online = 0;
       local->version = 0;
       local->cb_version = 0;
       prcu_cblist_init(&local->cblist);
}

/*
 * Initialize PRCU at boot time.
 */
void __init prcu_init(void)
{
       int cpu;

       open_softirq(PRCU_SOFTIRQ, prcu_process_callbacks);
       for_each_possible_cpu(cpu)
               prcu_init_local_struct(cpu);
}
