#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "priority.h"
#include "scheduler.h"
#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"

static prio_node_t *prio_queue = NULL;

static prio_node_t prio_node_pool[NPROC];
static prio_node_t *prio_free_nodes = NULL;

static uint32_t prio_queue_count = 0;

static bool aging_enabled = PRIO_AGING_ENABLED;
static uint32_t aging_interval = PRIO_AGING_INTERVAL;
static uint32_t aging_counter = 0;

static prio_stats_t prio_stats;

static uint64_t prio_ticks = 0;

static sid32 prio_lock;

extern proc_t proctab[];
extern pid32 currpid;
extern void context_switch(pid32 oldpid, pid32 newpid);

static scheduler_ops_t prio_ops = {
    .name = "Priority",
    .init = priority_init,
    .shutdown = priority_shutdown,
    .schedule = priority_schedule,
    .yield = priority_yield,
    .preempt = priority_preempt,
    .enqueue = priority_enqueue,
    .dequeue = priority_dequeue,
    .pick_next = priority_pick_next,
    .set_priority = priority_set,
    .get_priority = priority_get,
    .boost_priority = priority_boost,
    .decay_priority = priority_decay,
    .set_quantum = NULL,
    .get_quantum = NULL,
    .tick = priority_tick,
    .get_stats = NULL,
    .reset_stats = priority_reset_stats,
    .print_stats = priority_print_stats
};

static void prio_pool_init(void) {
    int i;
    
    prio_free_nodes = &prio_node_pool[0];
    for (i = 0; i < NPROC - 1; i++) {
        prio_node_pool[i].next = &prio_node_pool[i + 1];
        prio_node_pool[i].pid = -1;
    }
    prio_node_pool[NPROC - 1].next = NULL;
}

static prio_node_t *prio_node_alloc(void) {
    prio_node_t *node;
    
    if (prio_free_nodes == NULL) {
        return NULL;
    }
    
    node = prio_free_nodes;
    prio_free_nodes = prio_free_nodes->next;
    
    node->next = NULL;
    node->pid = -1;
    node->base_priority = PRIORITY_DEFAULT;
    node->current_priority = PRIORITY_DEFAULT;
    node->wait_time = 0;
    node->last_run = 0;
    node->cpu_burst = 0;
    node->io_bound = false;
    
    return node;
}

static void prio_node_free(prio_node_t *node) {
    if (node == NULL) {
        return;
    }
    
    node->next = prio_free_nodes;
    prio_free_nodes = node;
}

static prio_node_t *prio_find_node(pid32 pid) {
    prio_node_t *node = prio_queue;
    
    while (node != NULL) {
        if (node->pid == pid) {
            return node;
        }
        node = node->next;
    }
    
    return NULL;
}

void priority_init(void) {
    intmask mask;
    
    mask = disable();
    
    prio_pool_init();
    
    prio_queue = NULL;
    prio_queue_count = 0;
    
    aging_enabled = PRIO_AGING_ENABLED;
    aging_interval = PRIO_AGING_INTERVAL;
    aging_counter = 0;
    
    memset(&prio_stats, 0, sizeof(prio_stats));
    
    prio_lock = semcreate(1);
    
    prio_ticks = 0;
    
    restore(mask);
}

void priority_shutdown(void) {
    intmask mask;
    
    mask = disable();
    
    prio_queue = NULL;
    prio_queue_count = 0;
    
    restore(mask);
}

scheduler_ops_t *priority_get_ops(void) {
    return &prio_ops;
}

void priority_insert_ordered(pid32 pid) {
    prio_node_t *node, *prev, *curr;
    uint32_t priority;
    
    node = prio_node_alloc();
    if (node == NULL) {
        return;
    }
    
    node->pid = pid;
    node->base_priority = proctab[pid].pprio;
    node->current_priority = proctab[pid].pprio;
    node->wait_time = 0;
    node->last_run = prio_ticks;
    
    priority = node->current_priority;
    
    prev = NULL;
    curr = prio_queue;
    
    while (curr != NULL && curr->current_priority >= priority) {
        prev = curr;
        curr = curr->next;
    }
    
    node->next = curr;
    if (prev == NULL) {
        prio_queue = node;
    } else {
        prev->next = node;
    }
    
    prio_queue_count++;
    prio_stats.current_queue_length = prio_queue_count;
}

void priority_enqueue(pid32 pid) {
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    wait(prio_lock);
    
    if (prio_find_node(pid) != NULL) {
        signal(prio_lock);
        restore(mask);
        return;
    }
    
    priority_insert_ordered(pid);
    
    signal(prio_lock);
    restore(mask);
}

void priority_dequeue(pid32 pid) {
    prio_node_t *prev, *curr;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    wait(prio_lock);
    
    prev = NULL;
    curr = prio_queue;
    
    while (curr != NULL && curr->pid != pid) {
        prev = curr;
        curr = curr->next;
    }
    
    if (curr == NULL) {
        signal(prio_lock);
        restore(mask);
        return;
    }
    
    if (prev == NULL) {
        prio_queue = curr->next;
    } else {
        prev->next = curr->next;
    }
    
    prio_queue_count--;
    prio_stats.current_queue_length = prio_queue_count;
    
    prio_node_free(curr);
    
    signal(prio_lock);
    restore(mask);
}

pid32 priority_pick_next(void) {
    if (prio_queue == NULL) {
        return -1;
    }
    
    return prio_queue->pid;
}

void priority_schedule(void) {
    pid32 next_pid;
    pid32 old_pid;
    prio_node_t *next_node;
    intmask mask;
    
    mask = disable();
    
    prio_stats.total_schedules++;
    
    next_pid = priority_pick_next();
    
    if (next_pid < 0) {
        restore(mask);
        return;
    }
    
    if (next_pid != currpid) {
        old_pid = currpid;
        
        if (proctab[old_pid].pstate == PR_CURR) {
            proctab[old_pid].pstate = PR_READY;
        }
        
        proctab[next_pid].pstate = PR_CURR;
        currpid = next_pid;
        
        next_node = prio_find_node(next_pid);
        if (next_node != NULL) {

            prio_stats.avg_wait_time = 
                (prio_stats.avg_wait_time + next_node->wait_time) / 2;
            
            next_node->wait_time = 0;
            next_node->last_run = prio_ticks;
        }
        
        priority_dequeue(next_pid);
        
        prio_stats.context_switches++;
        
        context_switch(old_pid, next_pid);
    }
    
    restore(mask);
}

void priority_yield(void) {
    intmask mask;
    
    mask = disable();
    
    if (proctab[currpid].pstate == PR_CURR) {
        proctab[currpid].pstate = PR_READY;
        priority_enqueue(currpid);
    }
    
    priority_schedule();
    
    restore(mask);
}

void priority_preempt(void) {
    intmask mask;
    
    mask = disable();
    
    prio_stats.preemptions++;
    
    if (proctab[currpid].pstate == PR_CURR) {
        proctab[currpid].pstate = PR_READY;
        priority_enqueue(currpid);
    }
    
    priority_schedule();
    
    restore(mask);
}

void priority_set(pid32 pid, uint32_t priority) {
    prio_node_t *node;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    if (priority > PRIORITY_MAX) {
        priority = PRIORITY_MAX;
    }
    
    mask = disable();
    wait(prio_lock);
    
    proctab[pid].pprio = priority;
    
    node = prio_find_node(pid);
    if (node != NULL) {
        node->base_priority = priority;
        node->current_priority = priority;
        
        prio_node_t *prev = NULL;
        prio_node_t *curr = prio_queue;
        
        while (curr != NULL && curr != node) {
            prev = curr;
            curr = curr->next;
        }
        
        if (curr != NULL) {
            if (prev == NULL) {
                prio_queue = node->next;
            } else {
                prev->next = node->next;
            }
            prio_queue_count--;
            
            prio_node_free(node);
            priority_insert_ordered(pid);
        }
    }
    
    prio_stats.priority_changes++;
    
    signal(prio_lock);
    restore(mask);
    
    if (proctab[pid].pstate == PR_READY || pid == currpid) {
        extern volatile bool need_resched;
        need_resched = true;
    }
}

uint32_t priority_get(pid32 pid) {
    prio_node_t *node;
    uint32_t priority;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return 0;
    }
    
    mask = disable();
    
    node = prio_find_node(pid);
    if (node != NULL) {
        priority = node->current_priority;
    } else {
        priority = proctab[pid].pprio;
    }
    
    restore(mask);
    
    return priority;
}

void priority_boost(pid32 pid) {
    prio_node_t *node;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    
    node = prio_find_node(pid);
    if (node != NULL) {
        if (node->current_priority < PRIORITY_MAX) {
            node->current_priority++;
            
        }
    } else {
        if (proctab[pid].pprio < PRIORITY_MAX) {
            proctab[pid].pprio++;
        }
    }
    
    restore(mask);
}

void priority_decay(pid32 pid) {
    prio_node_t *node;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    
    node = prio_find_node(pid);
    if (node != NULL) {
        if (node->current_priority > node->base_priority) {
            node->current_priority--;
        }
    }
    
    restore(mask);
}

void priority_reset(pid32 pid) {
    prio_node_t *node;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    
    node = prio_find_node(pid);
    if (node != NULL) {
        node->current_priority = node->base_priority;
    }
    
    restore(mask);
}

void priority_age_all(void) {
    prio_node_t *node;
    intmask mask;
    
    if (!aging_enabled) {
        return;
    }
    
    mask = disable();
    
    node = prio_queue;
    while (node != NULL) {

        if (node->current_priority < PRIORITY_MAX) {
            node->current_priority += PRIO_AGING_AMOUNT;
            if (node->current_priority > PRIORITY_MAX) {
                node->current_priority = PRIORITY_MAX;
            }
            prio_stats.aging_boosts++;
        }
        node = node->next;
    }
    
    restore(mask);
}

void priority_check_starvation(void) {
    prio_node_t *node;
    intmask mask;
    
    mask = disable();
    
    node = prio_queue;
    while (node != NULL) {
        if (node->wait_time > PRIO_STARVATION_THRESHOLD) {

            node->current_priority += PRIO_STARVATION_BOOST;
            if (node->current_priority > PRIORITY_MAX) {
                node->current_priority = PRIORITY_MAX;
            }
            prio_stats.starvation_boosts++;
            node->wait_time = 0;
        }
        node = node->next;
    }
    
    restore(mask);
}

void priority_aging_enable(bool enable) {
    aging_enabled = enable;
}

void priority_set_aging_interval(uint32_t ticks) {
    aging_interval = ticks;
}

void priority_tick(void) {
    prio_node_t *node;
    intmask mask;
    
    mask = disable();
    
    prio_ticks++;
    
    node = prio_queue;
    while (node != NULL) {
        node->wait_time++;
        node = node->next;
    }
    
    if (aging_enabled) {
        aging_counter++;
        if (aging_counter >= aging_interval) {
            priority_age_all();
            aging_counter = 0;
        }
    }
    
    priority_check_starvation();
    
    if (prio_queue != NULL && currpid >= 0) {
        pid32 top_pid = prio_queue->pid;
        if (proctab[top_pid].pprio > proctab[currpid].pprio) {
            extern volatile bool need_resched;
            need_resched = true;
        }
    }
    
    restore(mask);
}

void priority_get_stats(prio_stats_t *stats) {
    intmask mask;
    
    if (stats == NULL) {
        return;
    }
    
    mask = disable();
    memcpy(stats, &prio_stats, sizeof(prio_stats_t));
    restore(mask);
}

void priority_reset_stats(void) {
    intmask mask;
    
    mask = disable();
    memset(&prio_stats, 0, sizeof(prio_stats_t));
    prio_stats.current_queue_length = prio_queue_count;
    restore(mask);
}

void priority_print_stats(void) {
    intmask mask;
    
    mask = disable();
    
    kprintf("\n=== Priority Scheduler Statistics ===\n");
    kprintf("Queue Length: %u\n", prio_stats.current_queue_length);
    kprintf("Total Schedules: %llu\n", prio_stats.total_schedules);
    kprintf("Context Switches: %llu\n", prio_stats.context_switches);
    kprintf("Priority Changes: %u\n", prio_stats.priority_changes);
    kprintf("Preemptions: %u\n", prio_stats.preemptions);
    kprintf("Aging Boosts: %u\n", prio_stats.aging_boosts);
    kprintf("Starvation Boosts: %u\n", prio_stats.starvation_boosts);
    kprintf("Avg Wait Time: %u ticks\n", prio_stats.avg_wait_time);
    kprintf("Aging: %s (interval: %u)\n", 
            aging_enabled ? "enabled" : "disabled", aging_interval);
    
    restore(mask);
}

void priority_print_queue(void) {
    prio_node_t *node;
    intmask mask;
    
    mask = disable();
    
    kprintf("\n=== Priority Queue ===\n");
    kprintf("Count: %u\n", prio_queue_count);
    kprintf("PID   BasePri  CurrPri  WaitTime  LastRun\n");
    kprintf("----  -------  -------  --------  -------\n");
    
    node = prio_queue;
    while (node != NULL) {
        kprintf("%4d  %7u  %7u  %8llu  %7llu\n",
                node->pid, node->base_priority, node->current_priority,
                node->wait_time, node->last_run);
        node = node->next;
    }
    
    kprintf("\n");
    
    restore(mask);
}

bool priority_validate(void) {
    prio_node_t *node, *prev;
    int count = 0;
    bool valid = true;
    intmask mask;
    
    mask = disable();
    
    prev = NULL;
    node = prio_queue;
    
    while (node != NULL) {
        count++;
        
        if (node->pid < 0 || node->pid >= NPROC) {
            kprintf("PRIO: Invalid PID %d\n", node->pid);
            valid = false;
        }
        
        if (prev != NULL && node->current_priority > prev->current_priority) {
            kprintf("PRIO: Priority order violation: %u > %u\n",
                    node->current_priority, prev->current_priority);
            valid = false;
        }
        
        prev = node;
        node = node->next;
        
        if (count > NPROC) {
            kprintf("PRIO: Queue corrupted (too many nodes)\n");
            valid = false;
            break;
        }
    }
    
    if (count != prio_queue_count) {
        kprintf("PRIO: Count mismatch: %d vs %u\n", count, prio_queue_count);
        valid = false;
    }
    
    restore(mask);
    
    return valid;
}

void priority_dump(void) {
    int i;
    intmask mask;
    
    mask = disable();
    
    kprintf("\n=== Process Priorities ===\n");
    kprintf("PID   State   Priority\n");
    kprintf("----  ------  --------\n");
    
    for (i = 0; i < NPROC; i++) {
        if (proctab[i].pstate != PR_FREE) {
            const char *state;
            switch (proctab[i].pstate) {
                case PR_CURR:  state = "CURR"; break;
                case PR_READY: state = "READY"; break;
                case PR_SLEEP: state = "SLEEP"; break;
                case PR_WAIT:  state = "WAIT"; break;
                case PR_SUSP:  state = "SUSP"; break;
                default:       state = "???"; break;
            }
            kprintf("%4d  %6s  %8u\n", i, state, proctab[i].pprio);
        }
    }
    
    kprintf("\n");
    
    restore(mask);
}
