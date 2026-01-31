
#ifndef _CFS_H_
#define _CFS_H_

#include <stdint.h>
#include <stdbool.h>
#include "scheduler.h"

/* Scheduling period */
#define CFS_TARGET_LATENCY      20

/* Minimum timeslice to prevent excessive context switching */
#define CFS_MIN_GRANULARITY     4

/* Default nice value (0 = normal priority) */
#define CFS_NICE_DEFAULT        0

/* Nice value range */
#define CFS_NICE_MIN            -20
#define CFS_NICE_MAX            19

/* Total nice levels */
#define CFS_NICE_LEVELS         40

/* Weight for nice value 0 */
#define CFS_WEIGHT_NICE0        1024

/* Enable vruntime bonus for tasks that sleep */
#define CFS_SLEEPER_BONUS       1

/* Vruntime scaling factor */
#define CFS_VRUNTIME_SCALE      20

/* CFS task control block */
typedef struct cfs_task {
    pid32       pid;
    int8_t      nice;               /* Priority: -20 to +19 */
    uint32_t    weight;             /* Scheduling weight from nice value */
    uint64_t    vruntime;           /* Virtual runtime (fairness metric) */
    uint64_t    exec_start;         /* Last time task started running */
    uint64_t    sum_exec;           /* Total execution time */
    uint64_t    prev_sum_exec;      /* Exec time at last timeslice start */
    uint64_t    sleep_start;        /* Time when task went to sleep */
    bool        on_rq;              /* True if task is on run queue */
    
    /* Red-black tree pointers */
    struct cfs_task *left;
    struct cfs_task *right;
    struct cfs_task *parent;
    int         color;
    
    /* Doubly-linked list for timeline */
    struct cfs_task *next;
    struct cfs_task *prev;
} cfs_task_t;

/* Run queue */
typedef struct cfs_rq {
    uint32_t    nr_running;         /* Number of runnable tasks */
    uint64_t    min_vruntime;       /* Minimum vruntime */
    uint64_t    clock;              /* Clock */
    uint64_t    clock_task;         /* Task clock */
    uint32_t    load_weight;        /* Sum of all task weights */
    
    cfs_task_t  *tasks_timeline;    /* Head of task list */
    cfs_task_t  *curr;              /* Currently running task */
    cfs_task_t  *next;              /* Next task to run */
    cfs_task_t  *leftmost;          /* Task with smallest vruntime (leftmost) (always lowest vruntime) */
} cfs_rq_t;

/* Scheduler statistics */
typedef struct cfs_stats {
    uint64_t    switches;           /* Number of context switches */
    uint64_t    total_runtime;      /* Total runtime */
    uint64_t    wait_time;          /* Total time spent waiting */
    uint64_t    sleep_time;         /* Total time spent sleeping */
    uint32_t    nr_migrations;      /* Number of task migrations */
    double      fairness_index;     /* Fairness index */
} cfs_stats_t;

/* Initialization and lifecycle */
void cfs_init(void);
void cfs_shutdown(void);
scheduler_ops_t *cfs_get_ops(void);

/* Core scheduling operations */
void cfs_schedule(void);
void cfs_yield(void);
void cfs_preempt(void);
cfs_task_t *cfs_pick_next_task(void);
bool cfs_check_preempt(void);

/* Task queue management */
void cfs_enqueue(pid32 pid);
void cfs_dequeue(pid32 pid);
void cfs_put_prev_task(void);
void cfs_set_curr_task(cfs_task_t *task);

/* Virtual runtime calculations */
void cfs_update_vruntime(cfs_task_t *task, uint64_t delta_exec);
uint64_t cfs_calc_delta(uint64_t delta_exec, uint32_t weight);
void cfs_update_min_vruntime(void);
void cfs_place_task(cfs_task_t *task, bool initial);

/* Priority and weight management */
int cfs_set_nice(pid32 pid, int nice);
int cfs_get_nice(pid32 pid);
uint32_t cfs_nice_to_weight(int nice);
uint32_t cfs_nice_to_wmult(int nice);

/* Timeslice calculations */
uint32_t cfs_timeslice(cfs_task_t *task);
uint32_t cfs_sched_latency(void);

/* Clock and timer */
void cfs_tick(void);
void cfs_update_clock(uint64_t delta);

/* Sleep/wake operations */
void cfs_sleep(pid32 pid);
void cfs_wakeup(pid32 pid);
uint64_t cfs_sleeper_credit(cfs_task_t *task, uint64_t sleep_time);

/* Statistics and debugging */
void cfs_get_stats(cfs_stats_t *stats);
void cfs_reset_stats(void);
void cfs_print_stats(void);
void cfs_print_rq(void);
void cfs_print_task(cfs_task_t *task);
bool cfs_validate(void);
cfs_task_t *cfs_get_task(pid32 pid);

#endif
