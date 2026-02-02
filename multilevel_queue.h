#ifndef _MULTILEVEL_QUEUE_H_
#define _MULTILEVEL_QUEUE_H_

#include <stdint.h>
#include <stdbool.h>
#include "scheduler.h"

#define MLFQ_NUM_LEVELS         8

#define MLFQ_BOOST_INTERVAL     1000

#define MLFQ_MAX_WAIT_TIME      500

#define MLFQ_IO_BONUS_LEVELS    2

typedef struct mlfq_node {
    pid32   pid;
    uint32_t level;
    uint32_t time_allotment;
    uint32_t time_used;
    uint64_t arrival_time;
    uint32_t io_count;
    struct mlfq_node *next;
    struct mlfq_node *prev;
} mlfq_node_t;

typedef struct mlfq_queue {
    mlfq_node_t *head;
    mlfq_node_t *tail;
    uint32_t count;
    uint32_t quantum;
    uint32_t allotment;
} mlfq_queue_t;

typedef struct mlfq_stats {
    uint64_t total_schedules;
    uint64_t context_switches;
    uint32_t promotions;
    uint32_t demotions;
    uint32_t priority_boosts;
    uint32_t io_bonuses;
    uint32_t per_level_count[MLFQ_NUM_LEVELS];
    uint64_t per_level_time[MLFQ_NUM_LEVELS];
} mlfq_stats_t;

void mlfq_init(void);

void mlfq_shutdown(void);

scheduler_ops_t *mlfq_get_ops(void);

void mlfq_schedule(void);

void mlfq_yield(void);

void mlfq_preempt(void);

void mlfq_enqueue(pid32 pid);

void mlfq_dequeue(pid32 pid);

pid32 mlfq_pick_next(void);

void mlfq_move_to_level(pid32 pid, uint32_t level);

void mlfq_demote(pid32 pid);

void mlfq_promote(pid32 pid);

void mlfq_priority_boost(void);

void mlfq_set_boost_interval(uint32_t ticks);

void mlfq_boost_enable(bool enable);

uint32_t mlfq_get_quantum(uint32_t level);

void mlfq_set_quantum(uint32_t level, uint32_t quantum);

void mlfq_tick(void);

void mlfq_io_done(pid32 pid);

void mlfq_io_bonus_enable(bool enable);

void mlfq_get_stats(mlfq_stats_t *stats);

void mlfq_reset_stats(void);

void mlfq_print_stats(void);

void mlfq_print_queues(void);

void mlfq_print_level(uint32_t level);

bool mlfq_validate(void);

int32_t mlfq_get_level(pid32 pid);

#define ml_queue_init       mlfq_init
#define ml_queue_schedule   mlfq_schedule
#define ml_queue_enqueue    mlfq_enqueue

#endif
