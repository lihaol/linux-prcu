#ifndef __LINUX_PRCU_H
#define __LINUX_PRCU_H

#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#define CONFIG_PRCU

struct prcu_version_head {
       unsigned long long version;
       struct prcu_version_head *next;
};

/* Simple unsegmented callback list for PRCU. */
struct prcu_cblist {
       struct rcu_head *head;
       struct rcu_head **tail;
       struct prcu_version_head *version_head;
       struct prcu_version_head **version_tail;
       long len;
};

#define PRCU_CBLIST_INITIALIZER(n) { \
       .head = NULL, .tail = &n.head, \
       .version_head = NULL, .version_tail = &n.version_head, \
}

struct prcu_local_struct {
       unsigned int locked;
       unsigned int online;
       unsigned long long version;
       unsigned long long cb_version;
       struct prcu_cblist cblist;
};

struct prcu_struct {
       atomic64_t global_version;
       atomic64_t cb_version;
       atomic_t active_ctr;
       struct mutex mtx;
       wait_queue_head_t wait_q;
};

#ifdef CONFIG_PRCU
void prcu_read_lock(void);
void prcu_read_unlock(void);
void synchronize_prcu(void);
void call_prcu(struct rcu_head *head, rcu_callback_t func);
void prcu_init(void);
void prcu_note_context_switch(void);
int prcu_pending(void);
void invoke_prcu_core(void);
void prcu_check_callbacks(void);

#else /* #ifdef CONFIG_PRCU */

#define prcu_read_lock() do {} while (0)
#define prcu_read_unlock() do {} while (0)
#define synchronize_prcu() do {} while (0)
#define call_prcu() do {} while (0)
#define prcu_init() do {} while (0)
#define prcu_note_context_switch() do {} while (0)
#define prcu_pending() 0
#define invoke_prcu_core() do {} while (0)
#define prcu_check_callbacks() do {} while (0)

#endif /* #ifdef CONFIG_PRCU */
#endif /* __LINUX_PRCU_H */
