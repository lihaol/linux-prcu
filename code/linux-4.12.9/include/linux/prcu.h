/*
 * Read-Copy Update mechanism for mutual exclusion (PRCU version).
 * PRCU public definitions.
 *
 * Authors: Heng Zhang <heng.z@huawei.com>
 *          Lihao Liang <lihao.liang@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_PRCU_H
#define __LINUX_PRCU_H

#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/completion.h>

#ifdef CONFIG_PRCU

/*
 * Simple list structure of callback versions.
 *
 * Note: Ideally, we would like to add the version field
 * to the rcu_head struct.  But if we do so, other users of
 * rcu_head in the Linux kernel will complain hard and loudly.
 */
struct prcu_version_head {
       unsigned long long version;
       struct prcu_version_head *next;
};

/*
 * Simple unsegmented callback list for PRCU.
 *
 * Note: Since we can't add a new version field to rcu_head,
 * we have to make our own callback list for PRCU instead of
 * using the existing rcu_cblist. Sigh!
 */
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

/*
 * PRCU's per-CPU state.
 */
struct prcu_local_struct {
       unsigned int locked;           /* Nesting level of PRCU read-side */
                                      /*  critcal sections */
       unsigned int online;           /* Indicates whether a context-switch */
                                      /*  has occurred on this CPU */
       unsigned long long version;    /* Local grace-period version */
       unsigned long long cb_version; /* Local callback version */
       struct rcu_head barrier_head;  /* PRCU callback list */
       struct prcu_cblist cblist;     /* PRCU callback version list */
};

/*
 * PRCU's global state.
 */
struct prcu_struct {
       atomic64_t global_version;            /* Global grace-period version */
       atomic64_t cb_version;                /* Global callback version */
       atomic_t active_ctr;                  /* Outstanding PRCU tasks */
                                             /*  being context-switched */
       atomic_t barrier_cpu_count;           /* # CPUs waiting on prcu_barrier() */
       struct mutex mtx;                     /* Serialize synchronize_prcu() */
       struct mutex barrier_mtx;             /* Serialize prcu_barrier() */
       wait_queue_head_t wait_q;             /* Wait for synchronize_prcu() */
       struct completion barrier_completion; /* Wait for prcu_barrier() */
};

/*
 * PRCU APIs.
 */
void prcu_read_lock(void);
void prcu_read_unlock(void);
void synchronize_prcu(void);
void call_prcu(struct rcu_head *head, rcu_callback_t func);
void prcu_barrier(void);

/*
 * Internal non-public functions.
 */
void prcu_init(void);
void prcu_note_context_switch(void);
int prcu_pending(void);
void invoke_prcu_core(void);
void prcu_check_callbacks(void);

#else /* #ifdef CONFIG_PRCU */

/*
 * If CONFIG_PRCU is not defined,
 * map its APIs to RCU's counterparts.
 */
#define prcu_read_lock rcu_read_lock
#define prcu_read_unlock rcu_read_unlock
#define synchronize_prcu synchronize_rcu
#define call_prcu call_rcu
#define prcu_barrier rcu_barrier

#define prcu_init() do {} while (0)
#define prcu_note_context_switch() do {} while (0)
#define prcu_pending() 0
#define invoke_prcu_core() do {} while (0)
#define prcu_check_callbacks() do {} while (0)

#endif /* #ifdef CONFIG_PRCU */
#endif /* __LINUX_PRCU_H */
