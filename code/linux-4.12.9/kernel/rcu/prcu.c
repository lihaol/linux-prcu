#include <linux/smp.h>
#include <linux/percpu.h>
#include <linux/prcu.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/barrier.h>

#include "rcu.h"

DEFINE_PER_CPU_SHARED_ALIGNED(struct prcu_local_struct, prcu_local);

struct prcu_struct global_prcu = {
       .global_version = ATOMIC64_INIT(0),
       .cb_version = ATOMIC64_INIT(0),
       .active_ctr = ATOMIC_INIT(0),
       .mtx = __MUTEX_INITIALIZER(global_prcu.mtx),
       .wait_q = __WAIT_QUEUE_HEAD_INITIALIZER(global_prcu.wait_q)
};
struct prcu_struct *prcu = &global_prcu;

/* Initialize simple callback list. */
static void prcu_cblist_init(struct prcu_cblist *rclp)
{
       rclp->head = NULL;
       rclp->tail = &rclp->head;
       rclp->version_head = NULL;
       rclp->version_tail = &rclp->version_head;
       rclp->len = 0;
}

/*
 * Dequeue the oldest rcu_head structure from the specified callback list;
 * store the callback grace period version number into the version pointer.
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

static inline void prcu_report(struct prcu_local_struct *local)
{
       unsigned long long global_version;
       unsigned long long local_version;

       global_version = atomic64_read(&prcu->global_version);
       local_version = local->version;
       if (global_version > local_version)
               cmpxchg(&local->version, local_version, global_version);
}

void prcu_read_lock(void)
{
       struct prcu_local_struct *local;

       local = get_cpu_ptr(&prcu_local);
       if (!local->online) {
               WRITE_ONCE(local->online, 1);
               smp_mb();
       }

       local->locked++;
       put_cpu_ptr(&prcu_local);
}
EXPORT_SYMBOL(prcu_read_lock);

void prcu_read_unlock(void)
{
       int locked;
       struct prcu_local_struct *local;

       barrier();
       local = get_cpu_ptr(&prcu_local);
       locked = local->locked;
       if (locked) {
               local->locked--;
               if (locked == 1)
                       prcu_report(local);
               put_cpu_ptr(&prcu_local);
       } else {
               put_cpu_ptr(&prcu_local);
               if (!atomic_dec_return(&prcu->active_ctr))
                       wake_up(&prcu->wait_q);
       }
}
EXPORT_SYMBOL(prcu_read_unlock);

static void prcu_handler(void *info)
{
       struct prcu_local_struct *local;

       local = this_cpu_ptr(&prcu_local);
       if (!local->locked)
               WRITE_ONCE(local->version, atomic64_read(&prcu->global_version));
}

void synchronize_prcu(void)
{
       int cpu;
       cpumask_t cpus;
       unsigned long long version;
       struct prcu_local_struct *local;

       version = atomic64_add_return(1, &prcu->global_version);
       mutex_lock(&prcu->mtx);

       local = get_cpu_ptr(&prcu_local);
       local->version = version;
       put_cpu_ptr(&prcu_local);

       cpumask_clear(&cpus);
       for_each_possible_cpu(cpu) {
               local = per_cpu_ptr(&prcu_local, cpu);
               if (!READ_ONCE(local->online))
                       continue;
               if (READ_ONCE(local->version) < version) {
                       smp_call_function_single(cpu, prcu_handler, NULL, 0);
                       cpumask_set_cpu(cpu, &cpus);
               }
       }

       for_each_cpu(cpu, &cpus) {
               local = per_cpu_ptr(&prcu_local, cpu);
               while (READ_ONCE(local->version) < version)
                       cpu_relax();
       }

       if (atomic_read(&prcu->active_ctr))
               wait_event(prcu->wait_q, !atomic_read(&prcu->active_ctr));

       atomic64_set(&prcu->cb_version, version);
       mutex_unlock(&prcu->mtx);
}
EXPORT_SYMBOL(synchronize_prcu);

void prcu_note_context_switch(void)
{
       struct prcu_local_struct *local;

       local = get_cpu_ptr(&prcu_local);
       if (local->locked) {
               atomic_add(local->locked, &prcu->active_ctr);
               local->locked = 0;
       }
       local->online = 0;
       prcu_report(local);
       put_cpu_ptr(&prcu_local);
}

void call_prcu(struct rcu_head *head, rcu_callback_t func)
{
       unsigned long flags;
       struct prcu_local_struct *local;
       struct prcu_cblist *rclp;
       struct prcu_version_head *vhp;

       debug_rcu_head_queue(head);

       /* Use GFP_ATOMIC with IRQs disabled */
       vhp = kmalloc(sizeof(struct prcu_version_head), GFP_ATOMIC);
       if (!vhp)
               return;

       head->func = func;
       head->next = NULL;
       vhp->next = NULL;

       local_irq_save(flags);
       local = this_cpu_ptr(&prcu_local);
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

int prcu_pending(void)
{
       struct prcu_local_struct *local = get_cpu_ptr(&prcu_local);
       unsigned long long cb_version = local->cb_version;
       struct prcu_cblist *rclp = &local->cblist;

       put_cpu_ptr(&prcu_local);
       return cb_version < atomic64_read(&prcu->cb_version) && rclp->head;
}

void invoke_prcu_core(void)
{
       if (cpu_online(smp_processor_id()))
               raise_softirq(PRCU_SOFTIRQ);
}

void prcu_check_callbacks(void)
{
       if (prcu_pending())
               invoke_prcu_core();
}

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

       /* Disable interrupts to prevent races with call_prcu() */
       local_irq_save(flags);
       local = this_cpu_ptr(&prcu_local);
       rclp = &local->cblist;
       rhp = rclp->head;
       vhp = rclp->version_head;
       for (; rhp && vhp && vhp->version < cb_version;
            rhp = rclp->head, vhp = rclp->version_head) {
               rhp = prcu_cblist_dequeue(rclp);
               debug_rcu_head_unqueue(rhp);
               rhp->func(rhp);
       }
       local->cb_version = cb_version;
       local_irq_restore(flags);
}

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

void __init prcu_init(void)
{
       int cpu;

       open_softirq(PRCU_SOFTIRQ, prcu_process_callbacks);
       for_each_possible_cpu(cpu)
               prcu_init_local_struct(cpu);
}
