#include <linux/smp.h>
#include <linux/prcu.h>
#include <linux/percpu.h>
#include <linux/compiler.h>
#include <linux/sched.h>

#include <asm/barrier.h>

DEFINE_PER_CPU_SHARED_ALIGNED(struct prcu_local_struct, prcu_local);

struct prcu_struct global_prcu = {
       .global_version = ATOMIC64_INIT(0),
       .active_ctr = ATOMIC_INIT(0),
       .mtx = __MUTEX_INITIALIZER(global_prcu.mtx),
       .wait_q = __WAIT_QUEUE_HEAD_INITIALIZER(global_prcu.wait_q)
};
struct prcu_struct *prcu = &global_prcu;

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
