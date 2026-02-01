#include "lottery.h"
#include "../include/kernel.h"
#include "../include/process.h"
#include <stdlib.h>
#include <string.h>

/* Linked list of all participants in the lottery */
static lottery_entry_t *lottery_pool = NULL;
static uint32_t total_tickets = 0;
static uint32_t participant_count = 0;
static bool compensation_enabled = LOTTERY_COMPENSATION_ENABLED;
static pid32 current_pid = -1;
static uint32_t time_remaining = 0;
static lottery_stats_t stats;
static uint32_t random_state = 1;
#define LOTTERY_MAX_ENTRIES 256
static lottery_entry_t entry_pool[LOTTERY_MAX_ENTRIES];
static lottery_entry_t *free_entries = NULL;
static scheduler_ops_t lottery_ops;
static lottery_entry_t *find_entry(pid32 pid);
static lottery_entry_t *alloc_entry(void);
static void free_entry(lottery_entry_t *entry);
static uint32_t random_next(void);
static uint32_t random_range(uint32_t max);
static void recalculate_totals(void);

/* Linear congruential generator for pseudo-random numbers */
static uint32_t random_next(void)
{
    /* Standard LCG constants */
    random_state = random_state * 1103515245 + 12345;
    return (random_state >> 16) & 0x7FFF;
}

/* Generate random number in range [0, max) */
static uint32_t random_range(uint32_t max)
{
    if (max == 0) {
        return 0;
    }
    return random_next() % max;
}

static lottery_entry_t *alloc_entry(void)
{
    if (free_entries == NULL) {
        return NULL;
    }
    
    lottery_entry_t *entry = free_entries;
    free_entries = free_entries->next;
    
    memset(entry, 0, sizeof(lottery_entry_t));
    return entry;
}

static void free_entry(lottery_entry_t *entry)
{
    if (entry == NULL) {
        return;
    }
    
    entry->next = free_entries;
    free_entries = entry;
}

static lottery_entry_t *find_entry(pid32 pid)
{
    lottery_entry_t *entry = lottery_pool;
    
    while (entry != NULL) {
        if (entry->pid == pid) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

/* Recalculate total tickets and participant count from scratch */
static void recalculate_totals(void)
{
    total_tickets = 0;
    participant_count = 0;
    
    lottery_entry_t *entry = lottery_pool;
    while (entry != NULL) {
        total_tickets += entry->current_tickets;
        participant_count++;
        entry = entry->next;
    }
    
    stats.total_tickets = total_tickets;
    stats.participant_count = participant_count;
}

/* Initialize the lottery scheduler */
void lottery_init(void)
{
    /* Build free list from entry pool */
    free_entries = NULL;
    for (int i = LOTTERY_MAX_ENTRIES - 1; i >= 0; i--) {
        entry_pool[i].next = free_entries;
        free_entries = &entry_pool[i];
    }
    
    lottery_pool = NULL;
    total_tickets = 0;
    participant_count = 0;
    current_pid = -1;
    time_remaining = 0;
    compensation_enabled = LOTTERY_COMPENSATION_ENABLED;
    
    memset(&stats, 0, sizeof(stats));
    stats.fairness_index = 1.0;
    
    random_state = 1;
    
    lottery_ops.init = lottery_init;
    lottery_ops.shutdown = lottery_shutdown;
    lottery_ops.schedule = lottery_schedule;
    lottery_ops.yield = lottery_yield;
    lottery_ops.preempt = lottery_preempt;
    lottery_ops.enqueue = lottery_enqueue;
    lottery_ops.dequeue = lottery_dequeue;
    lottery_ops.tick = lottery_tick;
    lottery_ops.get_stats = (void (*)(void *))lottery_get_stats;
    lottery_ops.print_stats = lottery_print_stats;
    lottery_ops.type = SCHED_LOTTERY;
    lottery_ops.name = "lottery";
}

void lottery_shutdown(void)
{

    lottery_entry_t *entry = lottery_pool;
    while (entry != NULL) {
        lottery_entry_t *next = entry->next;
        free_entry(entry);
        entry = next;
    }
    
    lottery_pool = NULL;
    total_tickets = 0;
    participant_count = 0;
    current_pid = -1;
}

scheduler_ops_t *lottery_get_ops(void)
{
    return &lottery_ops;
}

/* Draw a winning process from the lottery (proportional to ticket count) */
pid32 lottery_draw(void)
{
    if (lottery_pool == NULL || total_tickets == 0) {
        return -1;
    }
    
    /* Pick random ticket number */
    uint32_t winning_ticket = random_range(total_tickets);
    
    /* Walk through entries accumulating tickets until we find the winner */
    uint32_t counter = 0;
    lottery_entry_t *entry = lottery_pool;
    
    while (entry != NULL) {
        counter += entry->current_tickets;
        if (counter > winning_ticket) {
            /* Found the winner */
            entry->wins++;
            stats.total_lotteries++;
            return entry->pid;
        }
        entry = entry->next;
    }
    
    /* Fallback: should not reach here, but return first entry if we do */
    if (lottery_pool != NULL) {
        lottery_pool->wins++;
        stats.total_lotteries++;
        return lottery_pool->pid;
    }
    
    return -1;
}

/* Main scheduling decision: hold lottery and switch to winner */
void lottery_schedule(void)
{
    /* Don't preempt if current task still has time remaining */
    if (current_pid >= 0 && time_remaining > 0) {
        lottery_entry_t *current = find_entry(current_pid);
        if (current != NULL) {
            return;
        }
    }
    
    pid32 winner = lottery_draw();
    
    if (winner < 0) {
        /* No runnable tasks */
        return;
    }
    
    if (winner != current_pid) {
        pid32 old_pid = current_pid;
        current_pid = winner;
        time_remaining = DEFAULT_QUANTUM;
        
        extern void context_switch(pid32 old, pid32 new);
        context_switch(old_pid, winner);
    } else {
        /* Same task won again, reset quantum */
        time_remaining = DEFAULT_QUANTUM;
    }
}

/* Voluntarily yield CPU, award compensation tickets if enabled */
void lottery_yield(void)
{
    if (current_pid >= 0 && compensation_enabled) {
        /* Calculate fraction of quantum used, award compensation */
        float fraction = 1.0f - ((float)time_remaining / DEFAULT_QUANTUM);
        lottery_compensate(current_pid, fraction);
    }
    
    time_remaining = 0;
    lottery_schedule();
}

void lottery_preempt(void)
{
    time_remaining = 0;
    lottery_schedule();
}

/* Add a new process to the lottery pool */
void lottery_enqueue(pid32 pid)
{
    /* Don't add duplicates */
    if (find_entry(pid) != NULL) {
        return;
    }
    
    lottery_entry_t *entry = alloc_entry();
    if (entry == NULL) {
        return;
    }
    
    /* Initialize with default ticket allocation */
    entry->pid = pid;
    entry->base_tickets = LOTTERY_DEFAULT_TICKETS;
    entry->current_tickets = LOTTERY_DEFAULT_TICKETS;
    entry->compensation = 0;
    entry->wins = 0;
    entry->total_tickets_held = 0;
    
    entry->next = lottery_pool;
    lottery_pool = entry;
    
    total_tickets += entry->current_tickets;
    participant_count++;
    stats.total_tickets = total_tickets;
    stats.participant_count = participant_count;
}

/* Remove a process from the lottery pool permanently */
void lottery_dequeue(pid32 pid)
{
    lottery_entry_t *prev = NULL;
    lottery_entry_t *entry = lottery_pool;
    
    while (entry != NULL) {
        if (entry->pid == pid) {
            /* Remove from linked list */
            if (prev == NULL) {
                lottery_pool = entry->next;
            } else {
                prev->next = entry->next;
            }
            
            total_tickets -= entry->current_tickets;
            participant_count--;
            stats.total_tickets = total_tickets;
            stats.participant_count = participant_count;
            
            free_entry(entry);
            
            if (current_pid == pid) {
                current_pid = -1;
                time_remaining = 0;
            }
            
            return;
        }
        prev = entry;
        entry = entry->next;
    }
}

bool lottery_is_participant(pid32 pid)
{
    return find_entry(pid) != NULL;
}

/* Set base ticket count for a process (returns old value) */
uint32_t lottery_set_tickets(pid32 pid, uint32_t tickets)
{
    lottery_entry_t *entry = find_entry(pid);
    if (entry == NULL) {
        return 0;
    }
    
    /* Clamp to valid range */
    if (tickets < LOTTERY_MIN_TICKETS) {
        tickets = LOTTERY_MIN_TICKETS;
    }
    if (tickets > LOTTERY_MAX_TICKETS) {
        tickets = LOTTERY_MAX_TICKETS;
    }
    
    uint32_t old = entry->base_tickets;
    
    /* Update totals and apply compensation */
    total_tickets -= entry->current_tickets;
    
    entry->base_tickets = tickets;
    entry->current_tickets = tickets + entry->compensation;
    
    total_tickets += entry->current_tickets;
    stats.total_tickets = total_tickets;
    
    return old;
}

uint32_t lottery_get_tickets(pid32 pid)
{
    lottery_entry_t *entry = find_entry(pid);
    if (entry == NULL) {
        return 0;
    }
    return entry->current_tickets;
}

void lottery_add_tickets(pid32 pid, uint32_t tickets)
{
    lottery_entry_t *entry = find_entry(pid);
    if (entry == NULL) {
        return;
    }
    
    uint32_t new_tickets = entry->base_tickets + tickets;
    if (new_tickets > LOTTERY_MAX_TICKETS) {
        new_tickets = LOTTERY_MAX_TICKETS;
    }
    
    lottery_set_tickets(pid, new_tickets);
}

void lottery_remove_tickets(pid32 pid, uint32_t tickets)
{
    lottery_entry_t *entry = find_entry(pid);
    if (entry == NULL) {
        return;
    }
    
    uint32_t new_tickets;
    if (tickets >= entry->base_tickets) {
        new_tickets = LOTTERY_MIN_TICKETS;
    } else {
        new_tickets = entry->base_tickets - tickets;
    }
    
    lottery_set_tickets(pid, new_tickets);
}

/* Transfer tickets from one process to another (returns actual amount transferred) */
uint32_t lottery_transfer_tickets(pid32 from_pid, pid32 to_pid, uint32_t tickets)
{
    lottery_entry_t *from = find_entry(from_pid);
    lottery_entry_t *to = find_entry(to_pid);
    
    if (from == NULL || to == NULL) {
        return 0;
    }
    
    /* Can't go below minimum */
    uint32_t available = from->base_tickets - LOTTERY_MIN_TICKETS;
    uint32_t to_transfer = (tickets < available) ? tickets : available;
    
    if (to_transfer == 0) {
        return 0;
    }
    
    /* Can't exceed maximum */
    uint32_t space = LOTTERY_MAX_TICKETS - to->base_tickets;
    if (to_transfer > space) {
        to_transfer = space;
    }
    
    lottery_set_tickets(from_pid, from->base_tickets - to_transfer);
    lottery_set_tickets(to_pid, to->base_tickets + to_transfer);
    
    stats.tickets_transferred += to_transfer;
    
    return to_transfer;
}

/* Award compensation tickets to process that yielded early (formula: tickets * (1/fraction - 1)) */
void lottery_compensate(pid32 pid, float fraction_used)
{
    if (!compensation_enabled) {
        return;
    }
    
    lottery_entry_t *entry = find_entry(pid);
    if (entry == NULL) {
        return;
    }
    
    /* Compensation formula: tickets * (1/fraction_used - 1) */
    /* If process uses 50% of quantum, it gets 100% more tickets next time */
    
    if (fraction_used <= 0.0f || fraction_used >= 1.0f) {
        /* No compensation for full quantum usage or invalid values */
        total_tickets -= entry->compensation;
        entry->compensation = 0;
        entry->current_tickets = entry->base_tickets;
        total_tickets += entry->current_tickets;
    } else {
        /* Calculate compensation: shorter usage = more tickets */
        uint32_t comp = (uint32_t)(entry->base_tickets * (1.0f / fraction_used - 1.0f));
        
        total_tickets -= entry->compensation;
        entry->compensation = comp;
        entry->current_tickets = entry->base_tickets + comp;
        total_tickets += entry->current_tickets;
        
        stats.compensation_given += comp;
    }
    
    stats.total_tickets = total_tickets;
}

/* Enable or disable compensation tickets globally */
void lottery_compensation_enable(bool enable)
{
    compensation_enabled = enable;
    
    if (!enable) {
        /* Remove all compensation tickets when disabling */
        lottery_entry_t *entry = lottery_pool;
        while (entry != NULL) {
            total_tickets -= entry->compensation;
            entry->compensation = 0;
            entry->current_tickets = entry->base_tickets;
            entry = entry->next;
        }
        stats.total_tickets = total_tickets;
    }
}

/* Convert local (process-specific) tickets to global tickets based on current share */
uint32_t lottery_local_to_global(pid32 pid, uint32_t local_tickets)
{
    lottery_entry_t *entry = find_entry(pid);
    if (entry == NULL || total_tickets == 0) {
        return local_tickets;
    }
    
    /* Scale based on process's current share of total tickets */
    double share = (double)entry->current_tickets / total_tickets;
    return (uint32_t)(local_tickets * share);
}

/* Inflate or deflate all ticket counts by a factor (maintains relative proportions) */
void lottery_inflate(float factor)
{
    if (factor <= 0.0f) {
        return;
    }
    
    lottery_entry_t *entry = lottery_pool;
    while (entry != NULL) {
        uint32_t new_base = (uint32_t)(entry->base_tickets * factor);
        
        /* Clamp to valid range */
        if (new_base < LOTTERY_MIN_TICKETS) {
            new_base = LOTTERY_MIN_TICKETS;
        }
        if (new_base > LOTTERY_MAX_TICKETS) {
            new_base = LOTTERY_MAX_TICKETS;
        }
        
        entry->base_tickets = new_base;
        entry->current_tickets = new_base + entry->compensation;
        
        entry = entry->next;
    }
    
    recalculate_totals();
}

/* Timer tick: decrement quantum and reschedule if exhausted */
void lottery_tick(void)
{
    if (time_remaining > 0) {
        time_remaining--;
        
        /* Track cumulative tickets held over time */
        if (current_pid >= 0) {
            lottery_entry_t *entry = find_entry(current_pid);
            if (entry != NULL) {
                entry->total_tickets_held += entry->current_tickets;
            }
        }
    }
    
    if (time_remaining == 0) {
        /* Quantum exhausted, hold new lottery */
        lottery_schedule();
    }
}

void lottery_get_stats(lottery_stats_t *s)
{
    if (s == NULL) {
        return;
    }
    
    s->total_lotteries = stats.total_lotteries;
    s->total_tickets = total_tickets;
    s->participant_count = participant_count;
    s->tickets_transferred = stats.tickets_transferred;
    s->compensation_given = stats.compensation_given;
    s->fairness_index = lottery_fairness_index();
}

void lottery_reset_stats(void)
{
    stats.total_lotteries = 0;
    stats.tickets_transferred = 0;
    stats.compensation_given = 0;
    stats.fairness_index = 1.0;
    
    lottery_entry_t *entry = lottery_pool;
    while (entry != NULL) {
        entry->wins = 0;
        entry->total_tickets_held = 0;
        entry = entry->next;
    }
}

/* Calculate Jain's fairness index (1.0 = perfectly fair, closer to 0 = less fair) */
double lottery_fairness_index(void)
{
    if (participant_count < 2 || stats.total_lotteries == 0) {
        return 1.0;
    }
    
    /* Jain's fairness index: (sum(xi))^2 / (n * sum(xi^2)) */
    /* where xi is the ratio of actual to expected share */
    
    double sum = 0.0;
    double sum_sq = 0.0;
    
    lottery_entry_t *entry = lottery_pool;
    while (entry != NULL) {
        /* Compare actual win rate to expected win rate based on tickets */
        double expected_share = (double)entry->current_tickets / total_tickets;
        double actual_share = (double)entry->wins / stats.total_lotteries;
        
        double ratio = (expected_share > 0) ? actual_share / expected_share : 0;
        
        sum += ratio;
        sum_sq += ratio * ratio;
        
        entry = entry->next;
    }
    
    if (sum_sq == 0.0) {
        return 1.0;
    }
    
    stats.fairness_index = (sum * sum) / (participant_count * sum_sq);
    return stats.fairness_index;
}

void lottery_print_stats(void)
{
    kprintf("\n=== Lottery Scheduler Statistics ===\n");
    kprintf("Total lotteries held: %llu\n", stats.total_lotteries);
    kprintf("Total tickets in pool: %u\n", total_tickets);
    kprintf("Number of participants: %u\n", participant_count);
    kprintf("Tickets transferred: %u\n", stats.tickets_transferred);
    kprintf("Compensation tickets given: %u\n", stats.compensation_given);
    kprintf("Compensation enabled: %s\n", compensation_enabled ? "yes" : "no");
    kprintf("Fairness index: %.4f\n", lottery_fairness_index());
    
    kprintf("\nPer-process statistics:\n");
    lottery_entry_t *entry = lottery_pool;
    while (entry != NULL) {
        double expected = (double)entry->current_tickets / total_tickets * 100;
        double actual = (stats.total_lotteries > 0) ?
            (double)entry->wins / stats.total_lotteries * 100 : 0;
        
        kprintf("  PID %d: %u tickets (%u base + %u comp), "
                "%llu wins, expected %.1f%%, actual %.1f%%\n",
                entry->pid, entry->current_tickets,
                entry->base_tickets, entry->compensation,
                entry->wins, expected, actual);
        entry = entry->next;
    }
}

void lottery_print_pool(void)
{
    kprintf("\n=== Lottery Pool ===\n");
    kprintf("Total tickets: %u, Participants: %u\n\n",
            total_tickets, participant_count);
    
    lottery_entry_t *entry = lottery_pool;
    int idx = 0;
    uint32_t running_total = 0;
    
    while (entry != NULL) {
        running_total += entry->current_tickets;
        double percent = (total_tickets > 0) ?
            (double)entry->current_tickets / total_tickets * 100 : 0;
        
        kprintf("[%d] PID %d: %u tickets (%.2f%%), range [%u-%u]\n",
                idx++, entry->pid, entry->current_tickets, percent,
                running_total - entry->current_tickets,
                running_total - 1);
        entry = entry->next;
    }
    
    if (current_pid >= 0) {
        kprintf("\nCurrently running: PID %d, quantum remaining: %u\n",
                current_pid, time_remaining);
    }
}

/* Validate lottery pool invariants (for debugging) */
bool lottery_validate(void)
{
    bool valid = true;
    uint32_t counted_tickets = 0;
    uint32_t counted_participants = 0;
    
    /* Check each entry for consistency */
    lottery_entry_t *entry = lottery_pool;
    while (entry != NULL) {
        counted_tickets += entry->current_tickets;
        counted_participants++;
        
        if (entry->base_tickets < LOTTERY_MIN_TICKETS ||
            entry->base_tickets > LOTTERY_MAX_TICKETS) {
            kprintf("Lottery validation: PID %d has invalid base tickets %u\n",
                    entry->pid, entry->base_tickets);
            valid = false;
        }
        
        if (entry->current_tickets != entry->base_tickets + entry->compensation) {
            kprintf("Lottery validation: PID %d has inconsistent tickets "
                    "(current=%u, base=%u, comp=%u)\n",
                    entry->pid, entry->current_tickets,
                    entry->base_tickets, entry->compensation);
            valid = false;
        }
        
        entry = entry->next;
    }
    
    /* Verify global counters match actual values */
    if (counted_tickets != total_tickets) {
        kprintf("Lottery validation: ticket mismatch (counted=%u, stored=%u)\n",
                counted_tickets, total_tickets);
        valid = false;
    }
    
    if (counted_participants != participant_count) {
        kprintf("Lottery validation: participant mismatch (counted=%u, stored=%u)\n",
                counted_participants, participant_count);
        valid = false;
    }
    
    return valid;
}

/* Set random number generator seed (for reproducible testing) */
void lottery_set_seed(uint32_t seed)
{
    random_state = seed;
}
