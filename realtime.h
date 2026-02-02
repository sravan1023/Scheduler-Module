#ifndef _REALTIME_H_
#define _REALTIME_H_

#include <stdint.h>
#include <stdbool.h>
#include "scheduler.h"

#define RT_MAX_TASKS            64

typedef enum rt_algorithm {
    RT_ALGO_EDF,
    RT_ALGO_RMS,
    RT_ALGO_DMS,
    RT_ALGO_LLF,
} rt_algorithm_t;

#define RT_DEFAULT_ALGO         RT_ALGO_EDF

#define RT_DEFAULT_PERIOD       100

#define RT_DEFAULT_DEADLINE     100

#define RT_DEFAULT_WCET         10

typedef enum rt_miss_policy {
    RT_MISS_SKIP,
    RT_MISS_CONTINUE,
    RT_MISS_ABORT,
    RT_MISS_NOTIFY,
} rt_miss_policy_t;

#define RT_DEFAULT_MISS_POLICY  RT_MISS_NOTIFY

typedef enum rt_task_state {
    RT_STATE_INACTIVE,
    RT_STATE_READY,
    RT_STATE_RUNNING,
    RT_STATE_BLOCKED,
    RT_STATE_COMPLETED,
    RT_STATE_MISSED,
} rt_task_state_t;

typedef struct rt_task_params {
    uint32_t    period;
    uint32_t    deadline;
    uint32_t    wcet;
    uint32_t    phase;
    rt_miss_policy_t miss_policy;
} rt_task_params_t;

typedef struct rt_task {
    pid32               pid;
    rt_task_params_t    params;
    rt_task_state_t     state;
    
    uint64_t    release_time;
    uint64_t    absolute_deadline;
    uint64_t    remaining_time;
    uint64_t    start_time;
    
    uint64_t    instances;
    uint64_t    completions;
    uint64_t    deadline_misses;
    uint64_t    total_response_time;
    uint64_t    worst_response_time;
    uint64_t    total_exec_time;
    
    uint32_t    rms_priority;
    
    int64_t     laxity;
    
    struct rt_task  *next;
} rt_task_t;

typedef struct rt_stats {
    uint64_t    total_releases;
    uint64_t    total_completions;
    uint64_t    total_deadline_misses;
    uint64_t    preemptions;
    uint64_t    context_switches;
    double      utilization;
    double      schedulability_bound;
    bool        schedulable;
} rt_stats_t;

void realtime_init(void);

void realtime_shutdown(void);

scheduler_ops_t *realtime_get_ops(void);

void realtime_set_algorithm(rt_algorithm_t algo);

rt_algorithm_t realtime_get_algorithm(void);

void realtime_schedule(void);

void realtime_yield(void);

void realtime_preempt(void);

bool realtime_check_preempt(void);

rt_task_t *edf_pick_next(void);

void edf_enqueue(rt_task_t *task);

bool edf_check_schedulability(void);

rt_task_t *rms_pick_next(void);

void rms_assign_priorities(void);

bool rms_check_schedulability(void);

double rms_utilization_bound(uint32_t n);

rt_task_t *dms_pick_next(void);

void dms_assign_priorities(void);

rt_task_t *llf_pick_next(void);

void llf_update_laxity(void);

void realtime_enqueue(pid32 pid);

void realtime_dequeue(pid32 pid);

int realtime_create_task(pid32 pid, rt_task_params_t *params);

int realtime_set_params(pid32 pid, rt_task_params_t *params);

int realtime_get_params(pid32 pid, rt_task_params_t *params);

void realtime_release(rt_task_t *task);

void realtime_complete(pid32 pid);

bool realtime_check_deadline(rt_task_t *task);

void realtime_handle_miss(rt_task_t *task);

void realtime_tick(void);

void realtime_check_releases(void);

void realtime_check_deadlines(void);

double realtime_calc_utilization(void);

bool realtime_is_schedulable(void);

uint64_t realtime_response_time(rt_task_t *task);

void realtime_get_stats(rt_stats_t *stats);

void realtime_reset_stats(void);

void realtime_print_stats(void);

void realtime_print_tasks(void);

void realtime_print_task(rt_task_t *task);

bool realtime_validate(void);

rt_task_t *realtime_get_task(pid32 pid);

void realtime_set_time(uint64_t time);

uint64_t realtime_get_time(void);

#endif
