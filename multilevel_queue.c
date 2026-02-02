#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "multilevel_queue.h"
#include "scheduler.h"
#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"

static mlfq_queue_t mlfq_queues[MLFQ_NUM_LEVELS];

static mlfq_node_t mlfq_node_pool[NPROC];
static mlfq_node_t *mlfq_free_nodes = NULL;

static uint32_t level_quantums[MLFQ_NUM_LEVELS] = {
    MLFQ_Q0_QUANTUM, MLFQ_Q1_QUANTUM, MLFQ_Q2_QUANTUM, MLFQ_Q3_QUANTUM,
    MLFQ_Q4_QUANTUM, MLFQ_Q5_QUANTUM, MLFQ_Q6_QUANTUM, MLFQ_Q7_QUANTUM
};

static uint32_t level_allotments[MLFQ_NUM_LEVELS];

static bool boost_enabled = true;
static uint32_t boost_interval = MLFQ_BOOST_INTERVAL;
static uint32_t boost_counter = 0;

static bool io_bonus_enabled = true;

static mlfq_node_t *current_node = NULL;
static uint32_t current_time_used = 0;

static mlfq_stats_t mlfq_stats;

static uint64_t mlfq_ticks = 0;

static sid32 mlfq_lock;

extern proc_t proctab[];
extern pid32 currpid;
extern void context_switch(pid32 oldpid, pid32 newpid);

static scheduler_ops_t mlfq_ops = {
    .name = "Multi-Level Feedback Queue",
    .init = mlfq_init,
    .shutdown = mlfq_shutdown,
    .schedule = mlfq_schedule,
    .yield = mlfq_yield,
    .preempt = mlfq_preempt,
    .enqueue = mlfq_enqueue,
    .dequeue = mlfq_dequeue,
    .pick_next = mlfq_pick_next,
    .set_priority = NULL,
    .get_priority = NULL,
    .boost_priority = mlfq_promote,
    .decay_priority = mlfq_demote,
    .set_quantum = NULL,
    .get_quantum = NULL,
    .tick = mlfq_tick,
    .get_stats = NULL,
    .reset_stats = mlfq_reset_stats,
    .print_stats = mlfq_print_stats
};

static void mlfq_pool_init(void) {
    int i;
    
    mlfq_free_nodes = &mlfq_node_pool[0];
    for (i = 0; i < NPROC - 1; i++) {
        mlfq_node_pool[i].next = &mlfq_node_pool[i + 1];
        mlfq_node_pool[i].prev = NULL;
        mlfq_node_pool[i].pid = -1;
    }
    mlfq_node_pool[NPROC - 1].next = NULL;
}

static mlfq_node_t *mlfq_node_alloc(void) {
    mlfq_node_t *node;
    
    if (mlfq_free_nodes == NULL) {
        return NULL;
    }
    
    node = mlfq_free_nodes;
    mlfq_free_nodes = mlfq_free_nodes->next;
    
    node->next = NULL;
    node->prev = NULL;
    node->pid = -1;
    node->level = 0;
    node->time_allotment = level_allotments[0];
    node->time_used = 0;
    node->arrival_time = 0;
    node->io_count = 0;
    
    return node;
}

static void mlfq_node_free(mlfq_node_t *node) {
    if (node == NULL) {
        return;
    }
    
    node->next = mlfq_free_nodes;
    mlfq_free_nodes = node;
}

static mlfq_node_t *mlfq_find_node(pid32 pid, uint32_t *out_level) {
    int level;
    mlfq_node_t *node;
    
    for (level = 0; level < MLFQ_NUM_LEVELS; level++) {
        node = mlfq_queues[level].head;
        while (node != NULL) {
            if (node->pid == pid) {
                if (out_level != NULL) {
                }
                return node;
            }
            node = node->next;
        }
    }
    
    return NULL;
}

static void mlfq_add_to_level(mlfq_node_t *node, uint32_t level) {
    mlfq_queue_t *queue;
    
    if (level >= MLFQ_NUM_LEVELS) {
        level = MLFQ_NUM_LEVELS - 1;
    }
    
    queue = &mlfq_queues[level];
    
    node->level = level;
    node->next = NULL;
    node->prev = queue->tail;
    
    if (queue->tail != NULL) {
        queue->tail->next = node;
    } else {
        queue->head = node;
    }
    
    queue->tail = node;
    queue->count++;
    
    mlfq_stats.per_level_count[level]++;
}

static void mlfq_remove_from_queue(mlfq_node_t *node) {
    uint32_t level = node->level;
    mlfq_queue_t *queue = &mlfq_queues[level];
    
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        queue->head = node->next;
    }
    
    if (node->next != NULL) {
        node->next->prev = node->prev;
    } else {
        queue->tail = node->prev;
    }
    
    queue->count--;
    mlfq_stats.per_level_count[level]--;
    
    node->next = NULL;
    node->prev = NULL;
}

void mlfq_init(void) {
    int i;
    intmask mask;
    
    mask = disable();
    
    mlfq_pool_init();
    
    for (i = 0; i < MLFQ_NUM_LEVELS; i++) {
        mlfq_queues[i].head = NULL;
        mlfq_queues[i].tail = NULL;
        mlfq_queues[i].count = 0;
        mlfq_queues[i].quantum = level_quantums[i];
        mlfq_queues[i].allotment = level_quantums[i] * 2;
        level_allotments[i] = level_quantums[i] * 2;
    }
    
    boost_enabled = true;
    boost_interval = MLFQ_BOOST_INTERVAL;
    boost_counter = 0;
    
    io_bonus_enabled = true;
    
    current_node = NULL;
    current_time_used = 0;
    
    memset(&mlfq_stats, 0, sizeof(mlfq_stats));
    
    mlfq_lock = semcreate(1);
    
    mlfq_ticks = 0;
    
    restore(mask);
}

void mlfq_shutdown(void) {
    int i;
    intmask mask;
    
    mask = disable();
    
    for (i = 0; i < MLFQ_NUM_LEVELS; i++) {
        mlfq_queues[i].head = NULL;
        mlfq_queues[i].tail = NULL;
        mlfq_queues[i].count = 0;
    }
    
    current_node = NULL;
    
    restore(mask);
}

scheduler_ops_t *mlfq_get_ops(void) {
    return &mlfq_ops;
}

void mlfq_enqueue(pid32 pid) {
    mlfq_node_t *node;
    uint32_t start_level = 0;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    wait(mlfq_lock);
    
    if (mlfq_find_node(pid, NULL) != NULL) {
        signal(mlfq_lock);
        restore(mask);
        return;
    }
    
    node = mlfq_node_alloc();
    if (node == NULL) {
        signal(mlfq_lock);
        restore(mask);
        return;
    }
    
    if (proctab[pid].pprio >= 75) {
        start_level = 0;
    } else if (proctab[pid].pprio >= 50) {
        start_level = 2;
    } else if (proctab[pid].pprio >= 25) {
        start_level = 4;
    } else {
        start_level = 6;
    }
    
    node->pid = pid;
    node->time_allotment = level_allotments[start_level];
    node->time_used = 0;
    node->arrival_time = mlfq_ticks;
    node->io_count = 0;
    
    mlfq_add_to_level(node, start_level);
    
    signal(mlfq_lock);
    restore(mask);
}

void mlfq_dequeue(pid32 pid) {
    mlfq_node_t *node;
    uint32_t level;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    wait(mlfq_lock);
    
    node = mlfq_find_node(pid, &level);
    if (node == NULL) {
        signal(mlfq_lock);
        restore(mask);
        return;
    }
    
    if (current_node == node) {
        current_node = NULL;
    }
    
    mlfq_remove_from_queue(node);
    
    mlfq_node_free(node);
    
    signal(mlfq_lock);
    restore(mask);
}

pid32 mlfq_pick_next(void) {
    int level;
    
    for (level = 0; level < MLFQ_NUM_LEVELS; level++) {
        if (mlfq_queues[level].head != NULL) {
            return mlfq_queues[level].head->pid;
        }
    }
    
    return -1;
}

void mlfq_move_to_level(pid32 pid, uint32_t level) {
    mlfq_node_t *node;
    uint32_t old_level;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC || level >= MLFQ_NUM_LEVELS) {
        return;
    }
    
    mask = disable();
    
    node = mlfq_find_node(pid, &old_level);
    if (node == NULL) {
        restore(mask);
        return;
    }
    
    mlfq_remove_from_queue(node);
    
    node->time_allotment = level_allotments[level];
    node->time_used = 0;
    node->arrival_time = mlfq_ticks;
    
    mlfq_add_to_level(node, level);
    
    restore(mask);
}

void mlfq_demote(pid32 pid) {
    mlfq_node_t *node;
    uint32_t level;
    intmask mask;
    
    mask = disable();
    
    node = mlfq_find_node(pid, &level);
    if (node == NULL) {
        restore(mask);
        return;
    }
    
    if (level >= MLFQ_NUM_LEVELS - 1) {

        node->time_used = 0;
        node->time_allotment = level_allotments[level];
        restore(mask);
        return;
    }
    
    mlfq_remove_from_queue(node);
    
    level++;
    node->time_allotment = level_allotments[level];
    node->time_used = 0;
    node->arrival_time = mlfq_ticks;
    
    mlfq_add_to_level(node, level);
    
    mlfq_stats.demotions++;
    
    restore(mask);
}

void mlfq_promote(pid32 pid) {
    mlfq_node_t *node;
    uint32_t level;
    intmask mask;
    
    mask = disable();
    
    node = mlfq_find_node(pid, &level);
    if (node == NULL) {
        restore(mask);
        return;
    }
    
    if (level == 0) {
        restore(mask);
        return;
    }
    
    mlfq_remove_from_queue(node);
    
    level--;
    node->time_allotment = level_allotments[level];
    node->time_used = 0;
    node->arrival_time = mlfq_ticks;
    
    mlfq_add_to_level(node, level);
    
    mlfq_stats.promotions++;
    
    restore(mask);
}

void mlfq_schedule(void) {
    pid32 next_pid;
    pid32 old_pid;
    mlfq_node_t *next_node;
    uint32_t level;
    intmask mask;
    
    mask = disable();
    
    mlfq_stats.total_schedules++;
    
    next_pid = mlfq_pick_next();
    
    if (next_pid < 0) {
        restore(mask);
        return;
    }
    
    next_node = mlfq_find_node(next_pid, &level);
    
    if (next_pid != currpid) {
        old_pid = currpid;
        
        if (proctab[old_pid].pstate == PR_CURR) {
            proctab[old_pid].pstate = PR_READY;
        }
        
        proctab[next_pid].pstate = PR_CURR;
        currpid = next_pid;
        
        current_node = next_node;
        current_time_used = 0;
        
        mlfq_stats.context_switches++;
        mlfq_stats.per_level_time[level]++;
        
        context_switch(old_pid, next_pid);
    }
    
    restore(mask);
}

void mlfq_yield(void) {
    intmask mask;
    
    mask = disable();
    
    if (current_node != NULL) {

        current_node->io_count++;
        
        current_node->time_used = 0;
        
        if (io_bonus_enabled && current_node->io_count > 5) {
            mlfq_promote(current_node->pid);
            current_node->io_count = 0;
            mlfq_stats.io_bonuses++;
        }
    }
    
    if (proctab[currpid].pstate == PR_CURR) {
        proctab[currpid].pstate = PR_READY;
    }
    
    current_node = NULL;
    mlfq_schedule();
    
    restore(mask);
}

void mlfq_preempt(void) {
    intmask mask;
    
    mask = disable();
    
    if (current_node != NULL) {

        current_node->time_used += level_quantums[current_node->level];
        
        if (current_node->time_used >= current_node->time_allotment) {

            mlfq_demote(current_node->pid);
        } else {

            mlfq_remove_from_queue(current_node);
            mlfq_add_to_level(current_node, current_node->level);
        }
    }
    
    if (proctab[currpid].pstate == PR_CURR) {
        proctab[currpid].pstate = PR_READY;
    }
    
    current_node = NULL;
    mlfq_schedule();
    
    restore(mask);
}

void mlfq_priority_boost(void) {
    int level;
    mlfq_node_t *node, *next;
    intmask mask;
    
    mask = disable();
    wait(mlfq_lock);
    
    for (level = 1; level < MLFQ_NUM_LEVELS; level++) {
        node = mlfq_queues[level].head;
        while (node != NULL) {
            next = node->next;
            
            mlfq_remove_from_queue(node);
            
            node->time_allotment = level_allotments[0];
            node->time_used = 0;
            node->arrival_time = mlfq_ticks;
            
            mlfq_add_to_level(node, 0);
            
            node = next;
        }
    }
    
    mlfq_stats.priority_boosts++;
    
    signal(mlfq_lock);
    restore(mask);
}

void mlfq_set_boost_interval(uint32_t ticks) {
    boost_interval = ticks;
}

void mlfq_boost_enable(bool enable) {
    boost_enabled = enable;
}

uint32_t mlfq_get_quantum(uint32_t level) {
    if (level >= MLFQ_NUM_LEVELS) {
        level = MLFQ_NUM_LEVELS - 1;
    }
    return level_quantums[level];
}

void mlfq_set_quantum(uint32_t level, uint32_t quantum) {
    if (level >= MLFQ_NUM_LEVELS) {
        return;
    }
    
    level_quantums[level] = quantum;
    mlfq_queues[level].quantum = quantum;
    level_allotments[level] = quantum * 2;
    mlfq_queues[level].allotment = quantum * 2;
}

void mlfq_tick(void) {
    intmask mask;
    
    mask = disable();
    
    mlfq_ticks++;
    current_time_used++;
    
    if (current_node != NULL) {
        mlfq_stats.per_level_time[current_node->level]++;
        
        uint32_t quantum = level_quantums[current_node->level];
        if (current_time_used >= quantum) {

            extern volatile bool need_resched;
            need_resched = true;
        }
    }
    
    if (boost_enabled) {
        boost_counter++;
        if (boost_counter >= boost_interval) {
            mlfq_priority_boost();
            boost_counter = 0;
        }
    }
    
    restore(mask);
}

void mlfq_io_done(pid32 pid) {
    mlfq_node_t *node;
    uint32_t level;
    intmask mask;
    
    if (!io_bonus_enabled) {
        return;
    }
    
    mask = disable();
    
    node = mlfq_find_node(pid, &level);
    if (node != NULL) {
        node->io_count++;
        
        if (node->io_count > 3 && level > 0) {
            uint32_t new_level = level - MLFQ_IO_BONUS_LEVELS;
            if (new_level < 0 || new_level > level) {
                new_level = 0;
            }
            
            if (new_level != level) {
                mlfq_move_to_level(pid, new_level);
                mlfq_stats.io_bonuses++;
            }
            
            node->io_count = 0;
        }
    }
    
    restore(mask);
}

void mlfq_io_bonus_enable(bool enable) {
    io_bonus_enabled = enable;
}

void mlfq_get_stats(mlfq_stats_t *stats) {
    intmask mask;
    
    if (stats == NULL) {
        return;
    }
    
    mask = disable();
    memcpy(stats, &mlfq_stats, sizeof(mlfq_stats_t));
    restore(mask);
}

void mlfq_reset_stats(void) {
    int i;
    intmask mask;
    
    mask = disable();
    
    memset(&mlfq_stats, 0, sizeof(mlfq_stats_t));
    
    for (i = 0; i < MLFQ_NUM_LEVELS; i++) {
        mlfq_stats.per_level_count[i] = mlfq_queues[i].count;
    }
    
    restore(mask);
}

void mlfq_print_stats(void) {
    int i;
    intmask mask;
    
    mask = disable();
    
    kprintf("\n=== MLFQ Scheduler Statistics ===\n");
    kprintf("Total Schedules: %llu\n", mlfq_stats.total_schedules);
    kprintf("Context Switches: %llu\n", mlfq_stats.context_switches);
    kprintf("Promotions: %u\n", mlfq_stats.promotions);
    kprintf("Demotions: %u\n", mlfq_stats.demotions);
    kprintf("Priority Boosts: %u\n", mlfq_stats.priority_boosts);
    kprintf("I/O Bonuses: %u\n", mlfq_stats.io_bonuses);
    kprintf("Boost Interval: %u ticks\n", boost_interval);
    
    kprintf("\nPer-Level Statistics:\n");
    kprintf("Level  Quantum  Count  CPU Time\n");
    kprintf("-----  -------  -----  --------\n");
    for (i = 0; i < MLFQ_NUM_LEVELS; i++) {
        kprintf("%5d  %7u  %5u  %8llu\n",
                i, level_quantums[i], 
                mlfq_stats.per_level_count[i],
                mlfq_stats.per_level_time[i]);
    }
    
    restore(mask);
}

void mlfq_print_queues(void) {
    int i;
    intmask mask;
    
    mask = disable();
    
    kprintf("\n=== MLFQ Queues ===\n");
    
    for (i = 0; i < MLFQ_NUM_LEVELS; i++) {
        mlfq_print_level(i);
    }
    
    restore(mask);
}

void mlfq_print_level(uint32_t level) {
    mlfq_queue_t *queue;
    mlfq_node_t *node;
    intmask mask;
    
    if (level >= MLFQ_NUM_LEVELS) {
        return;
    }
    
    mask = disable();
    
    queue = &mlfq_queues[level];
    
    kprintf("\nLevel %u (quantum=%u, allotment=%u, count=%u):\n",
            level, queue->quantum, queue->allotment, queue->count);
    
    if (queue->head == NULL) {
        kprintf("  (empty)\n");
    } else {
        kprintf("  PID   TimeUsed  Allotment  I/O\n");
        kprintf("  ----  --------  ---------  ---\n");
        
        node = queue->head;
        while (node != NULL) {
            char marker = (node == current_node) ? '*' : ' ';
            kprintf("  %c%3d  %8u  %9u  %3u\n",
                    marker, node->pid, node->time_used,
                    node->time_allotment, node->io_count);
            node = node->next;
        }
    }
    
    restore(mask);
}

bool mlfq_validate(void) {
    int level;
    mlfq_node_t *node;
    uint32_t count;
    bool valid = true;
    intmask mask;
    
    mask = disable();
    
    for (level = 0; level < MLFQ_NUM_LEVELS; level++) {
        mlfq_queue_t *queue = &mlfq_queues[level];
        count = 0;
        
        node = queue->head;
        while (node != NULL) {
            count++;
            
            if (node->pid < 0 || node->pid >= NPROC) {
                kprintf("MLFQ: Invalid PID %d at level %d\n", node->pid, level);
                valid = false;
            }
            
            if (node->level != level) {
                kprintf("MLFQ: Level mismatch: node says %u, queue is %d\n",
                        node->level, level);
                valid = false;
            }
            
            if (node->next != NULL && node->next->prev != node) {
                kprintf("MLFQ: Link mismatch at PID %d\n", node->pid);
                valid = false;
            }
            
            node = node->next;
            
            if (count > NPROC) {
                kprintf("MLFQ: Queue %d appears corrupted\n", level);
                valid = false;
                break;
            }
        }
        
        if (count != queue->count) {
            kprintf("MLFQ: Count mismatch at level %d: %u vs %u\n",
                    level, count, queue->count);
            valid = false;
        }
    }
    
    restore(mask);
    
    return valid;
}

int32_t mlfq_get_level(pid32 pid) {
    mlfq_node_t *node;
    uint32_t level;
    intmask mask;
    
    mask = disable();
    
    node = mlfq_find_node(pid, &level);
    
    restore(mask);
    
    if (node == NULL) {
        return -1;
    }
    
    return (int32_t)level;
}
