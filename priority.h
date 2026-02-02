#ifndef _PRIORITY_H_
#define _PRIORITY_H_

#include <stdint.h>
#include <stdbool.h>
#include "scheduler.h"

#define PRIO_NUM_LEVELS         100

#define PRIO_IO_BONUS           5

typedef struct prio_node {
    pid32   pid;
    uint32_t base_priority;
    uint32_t current_priority;
    uint64_t wait_time;
    uint64_t last_run;
    uint32_t cpu_burst;
    bool    io_bound;
    struct prio_node *next;
} prio_node_t;

typedef struct prio_stats {
    uint64_t total_schedules;
    uint64_t context_switches;
    uint32_t priority_changes;
    uint32_t aging_boosts;
    uint32_t starvation_boosts;
    uint32_t preemptions;
    uint32_t current_queue_length;
    uint32_t avg_wait_time;
} prio_stats_t;

void priority_init(void);

void priority_shutdown(void);

scheduler_ops_t *priority_get_ops(void);

void priority_schedule(void);

void priority_yield(void);

void priority_preempt(void);

void priority_enqueue(pid32 pid);

void priority_dequeue(pid32 pid);

pid32 priority_pick_next(void);

void priority_insert_ordered(pid32 pid);

void priority_set(pid32 pid, uint32_t priority);

uint32_t priority_get(pid32 pid);

void priority_boost(pid32 pid);

void priority_decay(pid32 pid);

void priority_reset(pid32 pid);

void priority_age_all(void);

void priority_check_starvation(void);

void priority_aging_enable(bool enable);

void priority_set_aging_interval(uint32_t ticks);

void priority_tick(void);

void priority_get_stats(prio_stats_t *stats);

void priority_reset_stats(void);

void priority_print_stats(void);

void priority_print_queue(void);

bool priority_validate(void);

void priority_dump(void);

#endif
