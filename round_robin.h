#ifndef _ROUND_ROBIN_H_
#define _ROUND_ROBIN_H_

#include <stdint.h>
#include <stdbool.h>
#include "scheduler.h"

#define RR_DEFAULT_QUANTUM      10

#define RR_MIN_QUANTUM          1

#define RR_MAX_QUANTUM          100

typedef struct rr_node {
    pid32   pid;
    uint32_t time_remaining;
    uint64_t total_time;
    uint32_t rounds;
    struct rr_node *next;
    struct rr_node *prev;
} rr_node_t;

typedef struct rr_stats {
    uint32_t total_processes;
    uint64_t total_context_switches;
    uint64_t total_quantum_expires;
    uint32_t avg_wait_time;
    uint32_t current_queue_length;
    uint32_t max_queue_length;
} rr_stats_t;

void round_robin_init(void);

void round_robin_shutdown(void);

scheduler_ops_t *round_robin_get_ops(void);

void round_robin_schedule(void);

void round_robin_yield(void);

void round_robin_preempt(void);

void round_robin_enqueue(pid32 pid);

void round_robin_dequeue(pid32 pid);

pid32 round_robin_pick_next(void);

void round_robin_rotate(void);

void round_robin_set_quantum(uint32_t quantum);

uint32_t round_robin_get_quantum(void);

void round_robin_tick(void);

void round_robin_reset_slice(pid32 pid);

void round_robin_get_stats(rr_stats_t *stats);

void round_robin_reset_stats(void);

void round_robin_print_stats(void);

void round_robin_print_queue(void);

bool round_robin_validate(void);

#endif
