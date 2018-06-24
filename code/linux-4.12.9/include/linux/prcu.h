#ifndef __LINUX_PRCU_H
#define __LINUX_PRCU_H

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#define CONFIG_PRCU

struct prcu_local_struct {
       unsigned int locked;
       unsigned int online;
       unsigned long long version;
};

struct prcu_struct {
       atomic64_t global_version;
       atomic_t active_ctr;
       struct mutex mtx;
       wait_queue_head_t wait_q;
};

#ifdef CONFIG_PRCU
void prcu_read_lock(void);
void prcu_read_unlock(void);
void synchronize_prcu(void);
void prcu_note_context_switch(void);

#else /* #ifdef CONFIG_PRCU */

#define prcu_read_lock() do {} while (0)
#define prcu_read_unlock() do {} while (0)
#define synchronize_prcu() do {} while (0)
#define prcu_note_context_switch() do {} while (0)

#endif /* #ifdef CONFIG_PRCU */
#endif /* __LINUX_PRCU_H */
