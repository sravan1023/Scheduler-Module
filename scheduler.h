#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

#include <stdint.h>
#include <stdbool.h>
#include "../include/kernel.h"
#include "../include/process.h"

#define DEFAULT_QUANTUM         10

#define MIN_QUANTUM             1
#define MAX_QUANTUM             1000

#define DEFAULT_SCHED_POLICY    SCHED_PRIORITY

#define PRIORITY_MIN            0
#define PRIORITY_MAX            99
#define PRIORITY_DEFAULT        50
#define PRIORITY_IDLE           0
#define PRIORITY_LOW            25
#define PRIORITY_NORMAL         50
#define PRIORITY_HIGH           75
#define PRIORITY_REALTIME       99

    uint64_t    total_waittime;
    uint64_t    total_sleeptime;
    uint32_t    context_switches;
    uint32_t    voluntary_switches;
    uint32_t    involuntary_switches;
    uint32_t    time_slices;
    uint32_t    times_scheduled;
    uint64_t    last_scheduled;
    uint64_t    last_runtime;
} sched_proc_stats_t;

typedef struct sched_stats {
    uint64_t    total_schedules;
    uint64_t    context_switches;
    uint64_t    idle_time;
    uint64_t    busy_time;
    uint32_t    runnable_count;
    uint32_t    blocked_count;
    uint32_t    max_runnable;
    uint32_t    preemptions;
    uint32_t    voluntary_yields;
    uint64_t    quantum_expirations;
    uint64_t    avg_wait_time;
    uint64_t    avg_turnaround;
} sched_stats_t;

typedef struct ready_node {
    pid32   pid;
    uint32_t priority;
    uint32_t time_slice;
    uint64_t enqueue_time;
    struct ready_node *next;
    struct ready_node *prev;
} ready_node_t;

typedef struct ready_queue {
    ready_node_t *head;
    ready_node_t *tail;
    uint32_t count;
    uint32_t priority;
} ready_queue_t;

typedef struct scheduler_ops {
    const char *name;
    
    void (*init)(void);
    void (*shutdown)(void);
    
    void (*schedule)(void);
    void (*yield)(void);
    void (*preempt)(void);
    
    void (*enqueue)(pid32 pid);
    void (*dequeue)(pid32 pid);
    pid32 (*pick_next)(void);
    
    void (*set_priority)(pid32 pid, uint32_t prio);
    uint32_t (*get_priority)(pid32 pid);
    void (*boost_priority)(pid32 pid);
    void (*decay_priority)(pid32 pid);
    
    void (*set_quantum)(uint32_t quantum);
    uint32_t (*get_quantum)(void);
    void (*tick)(void);
    
    void (*get_stats)(sched_stats_t *stats);
    void (*reset_stats)(void);
    void (*print_stats)(void);
} scheduler_ops_t;

extern scheduler_ops_t *current_scheduler;

extern uint32_t sched_policy;

extern sched_stats_t sched_stats;

extern ready_queue_t ready_queue;

extern pid32 currpid;

extern volatile bool need_resched;

void scheduler_init(scheduler_type_t type);

void scheduler_shutdown(void);

syscall scheduler_switch(scheduler_type_t type);

void schedule(void);

void resched(void);

void yield(void);

void preempt(void);

void ready_queue_init(void);

void ready_enqueue(pid32 pid);

void ready_dequeue(pid32 pid);

pid32 ready_peek(void);

pid32 ready_pop(void);

bool ready_queue_empty(void);

uint32_t ready_queue_count(void);

syscall setpriority(pid32 pid, uint32_t priority);

syscall getpriority(pid32 pid);

syscall nice(int32_t increment);

void sched_set_quantum(uint32_t quantum);

uint32_t sched_get_quantum(void);

void sched_tick(void);

uint64_t sched_get_time(void);

void sched_ready(pid32 pid);

void sched_block(pid32 pid);

void sched_wakeup(pid32 pid);

void sched_new_process(pid32 pid);

void sched_exit(pid32 pid);

void sched_get_stats(sched_stats_t *stats);

void sched_get_proc_stats(pid32 pid, sched_proc_stats_t *stats);

void sched_reset_stats(void);

void sched_print_stats(void);

void sched_print_ready_queue(void);

bool sched_validate(void);

void sched_dump(void);

const char *sched_get_name(void);

extern void context_switch(pid32 oldpid, pid32 newpid);

extern void save_context(void);

extern void restore_context(pid32 pid);

#endif
