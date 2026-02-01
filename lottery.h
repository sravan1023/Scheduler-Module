#ifndef _LOTTERY_H_
#define _LOTTERY_H_

#include <stdint.h>
#include <stdbool.h>
#include "scheduler.h"

#define LOTTERY_DEFAULT_TICKETS     100

#define LOTTERY_MIN_TICKETS         1

#define LOTTERY_MAX_TICKETS         10000

#define LOTTERY_LOW_TICKETS         25
#define LOTTERY_NORMAL_TICKETS      100
#define LOTTERY_HIGH_TICKETS        400
#define LOTTERY_REALTIME_TICKETS    1600

#define LOTTERY_COMPENSATION_ENABLED    1

typedef struct lottery_entry {
    pid32   pid;
    uint32_t base_tickets;
    uint32_t current_tickets;
    uint32_t compensation;
    uint64_t wins;
    uint64_t total_tickets_held;
    struct lottery_entry *next;
} lottery_entry_t;

typedef struct lottery_stats {
    uint64_t total_lotteries;
    uint64_t total_tickets;
    uint32_t participant_count;
    uint32_t tickets_transferred;
    uint32_t compensation_given;
    double   fairness_index;
} lottery_stats_t;

void lottery_init(void);

void lottery_shutdown(void);

scheduler_ops_t *lottery_get_ops(void);

void lottery_schedule(void);

void lottery_yield(void);

void lottery_preempt(void);

pid32 lottery_draw(void);

void lottery_enqueue(pid32 pid);

void lottery_dequeue(pid32 pid);

bool lottery_is_participant(pid32 pid);

uint32_t lottery_set_tickets(pid32 pid, uint32_t tickets);

uint32_t lottery_get_tickets(pid32 pid);

void lottery_add_tickets(pid32 pid, uint32_t tickets);

void lottery_remove_tickets(pid32 pid, uint32_t tickets);

uint32_t lottery_transfer_tickets(pid32 from_pid, pid32 to_pid, uint32_t tickets);

void lottery_compensate(pid32 pid, float fraction_used);

void lottery_compensation_enable(bool enable);

uint32_t lottery_local_to_global(pid32 pid, uint32_t local_tickets);

void lottery_inflate(float factor);

void lottery_tick(void);

void lottery_get_stats(lottery_stats_t *stats);

void lottery_reset_stats(void);

void lottery_print_stats(void);

double lottery_fairness_index(void);

void lottery_print_pool(void);

bool lottery_validate(void);

void lottery_set_seed(uint32_t seed);

#endif
