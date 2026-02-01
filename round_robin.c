
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "round_robin.h"
#include "scheduler.h"
#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"

static rr_node_t *rr_queue_head = NULL;
static rr_node_t *rr_current = NULL;

static rr_node_t rr_node_pool[NPROC];
static rr_node_t *rr_free_nodes = NULL;

static uint32_t rr_queue_count = 0;

static uint32_t rr_quantum = RR_DEFAULT_QUANTUM;
static uint32_t rr_quantum_remaining = RR_DEFAULT_QUANTUM;

static rr_stats_t rr_stats;

static sid32 rr_lock;

extern proc_t proctab[];
extern pid32 currpid;
extern void context_switch(pid32 oldpid, pid32 newpid);

static scheduler_ops_t rr_ops = {
    .name = "Round-Robin",
    .init = round_robin_init,
    .shutdown = round_robin_shutdown,
    .schedule = round_robin_schedule,
    .yield = round_robin_yield,
    .preempt = round_robin_preempt,
    .enqueue = round_robin_enqueue,
    .dequeue = round_robin_dequeue,
    .pick_next = round_robin_pick_next,
    .set_priority = NULL,
    .get_priority = NULL,
    .boost_priority = NULL,
    .decay_priority = NULL,
    .set_quantum = round_robin_set_quantum,
    .get_quantum = round_robin_get_quantum,
    .tick = round_robin_tick,
    .get_stats = NULL,
    .reset_stats = round_robin_reset_stats,
    .print_stats = round_robin_print_stats
};

static void rr_pool_init(void) {
    int i;
    
    rr_free_nodes = &rr_node_pool[0];
    for (i = 0; i < NPROC - 1; i++) {
        rr_node_pool[i].next = &rr_node_pool[i + 1];
        rr_node_pool[i].prev = NULL;
        rr_node_pool[i].pid = -1;
    }
    rr_node_pool[NPROC - 1].next = NULL;
}

static rr_node_t *rr_node_alloc(void) {
    rr_node_t *node;
    
    if (rr_free_nodes == NULL) {
        return NULL;
    }
    
    node = rr_free_nodes;
    rr_free_nodes = rr_free_nodes->next;
    
    node->next = NULL;
    node->prev = NULL;
    node->pid = -1;
    node->time_remaining = rr_quantum;
    node->total_time = 0;
    node->rounds = 0;
    
    return node;
}

static void rr_node_free(rr_node_t *node) {
    if (node == NULL) {
        return;
    }
    
    node->next = rr_free_nodes;
    rr_free_nodes = node;
}

static rr_node_t *rr_find_node(pid32 pid) {
    rr_node_t *node;
    
    if (rr_queue_head == NULL) {
        return NULL;
    }
    
    node = rr_queue_head;
    do {
        if (node->pid == pid) {
            return node;
        }
        node = node->next;
    } while (node != rr_queue_head);
    
    return NULL;
}

void round_robin_init(void) {
    intmask mask;
    
    mask = disable();
    
    rr_pool_init();
    
    rr_queue_head = NULL;
    rr_current = NULL;
    rr_queue_count = 0;
    
    rr_quantum = RR_DEFAULT_QUANTUM;
    rr_quantum_remaining = RR_DEFAULT_QUANTUM;
    
    memset(&rr_stats, 0, sizeof(rr_stats));
    
    rr_lock = semcreate(1);
    
    restore(mask);
}

void round_robin_shutdown(void) {
    intmask mask;
    
    mask = disable();
    
    rr_queue_head = NULL;
    rr_current = NULL;
    rr_queue_count = 0;
    
    restore(mask);
}

scheduler_ops_t *round_robin_get_ops(void) {
    return &rr_ops;
}

void round_robin_enqueue(pid32 pid) {
    rr_node_t *node;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    wait(rr_lock);
    
    if (rr_find_node(pid) != NULL) {
        signal(rr_lock);
        restore(mask);
        return;
    }
    
    node = rr_node_alloc();
    if (node == NULL) {
        signal(rr_lock);
        restore(mask);
        return;
    }
    
    node->pid = pid;
    node->time_remaining = rr_quantum;
    
    if (rr_queue_head == NULL) {

        node->next = node;
        node->prev = node;
        rr_queue_head = node;
        rr_current = node;
    } else {

        node->next = rr_queue_head;
        node->prev = rr_queue_head->prev;
        rr_queue_head->prev->next = node;
        rr_queue_head->prev = node;
    }
    
    rr_queue_count++;
    rr_stats.total_processes++;
    
    if (rr_queue_count > rr_stats.max_queue_length) {
        rr_stats.max_queue_length = rr_queue_count;
    }
    rr_stats.current_queue_length = rr_queue_count;
    
    signal(rr_lock);
    restore(mask);
}

void round_robin_dequeue(pid32 pid) {
    rr_node_t *node;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    wait(rr_lock);
    
    node = rr_find_node(pid);
    if (node == NULL) {
        signal(rr_lock);
        restore(mask);
        return;
    }
    
    if (node->next == node) {
        rr_queue_head = NULL;
        rr_current = NULL;
    } else {

        node->prev->next = node->next;
        node->next->prev = node->prev;
        
        if (node == rr_queue_head) {
            rr_queue_head = node->next;
        }
        
        if (node == rr_current) {
            rr_current = node->next;
        }
    }
    
    rr_queue_count--;
    rr_stats.current_queue_length = rr_queue_count;
    
    rr_node_free(node);
    
    signal(rr_lock);
    restore(mask);
}

pid32 round_robin_pick_next(void) {
    if (rr_current == NULL) {
        return -1;
    }
    
    return rr_current->pid;
}

void round_robin_rotate(void) {
    intmask mask;
    
    mask = disable();
    
    if (rr_current != NULL && rr_queue_count > 1) {

        rr_current->rounds++;
        
        rr_current = rr_current->next;
        rr_current->time_remaining = rr_quantum;
    }
    
    restore(mask);
}

void round_robin_schedule(void) {
    pid32 next_pid;
    pid32 old_pid;
    intmask mask;
    
    mask = disable();
    
    next_pid = round_robin_pick_next();
    
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
        
        rr_stats.total_context_switches++;
        
        context_switch(old_pid, next_pid);
    }
    
    restore(mask);
}

void round_robin_yield(void) {
    intmask mask;
    
    mask = disable();
    
    if (rr_current != NULL) {
        rr_current->time_remaining = 0;
    }
    
    if (proctab[currpid].pstate == PR_CURR) {
        proctab[currpid].pstate = PR_READY;
    }
    
    round_robin_rotate();
    round_robin_schedule();
    
    restore(mask);
}

void round_robin_preempt(void) {

    round_robin_yield();
}

void round_robin_set_quantum(uint32_t quantum) {
    if (quantum < RR_MIN_QUANTUM) {
        quantum = RR_MIN_QUANTUM;
    }
    if (quantum > RR_MAX_QUANTUM) {
        quantum = RR_MAX_QUANTUM;
    }
    
    rr_quantum = quantum;
}

uint32_t round_robin_get_quantum(void) {
    return rr_quantum;
}

void round_robin_tick(void) {
    intmask mask;
    
    mask = disable();
    
    if (rr_current != NULL && rr_current->pid == currpid) {
        rr_current->total_time++;
        
        if (rr_current->time_remaining > 0) {
            rr_current->time_remaining--;
        }
        
        if (rr_current->time_remaining == 0) {
            rr_stats.total_quantum_expires++;
            
            round_robin_rotate();
            
            extern volatile bool need_resched;
            need_resched = true;
        }
    }
    
    restore(mask);
}

void round_robin_reset_slice(pid32 pid) {
    rr_node_t *node;
    intmask mask;
    
    mask = disable();
    
    node = rr_find_node(pid);
    if (node != NULL) {
        node->time_remaining = rr_quantum;
    }
    
    restore(mask);
}

void round_robin_get_stats(rr_stats_t *stats) {
    intmask mask;
    
    if (stats == NULL) {
        return;
    }
    
    mask = disable();
    memcpy(stats, &rr_stats, sizeof(rr_stats_t));
    restore(mask);
}

void round_robin_reset_stats(void) {
    intmask mask;
    
    mask = disable();
    memset(&rr_stats, 0, sizeof(rr_stats_t));
    rr_stats.current_queue_length = rr_queue_count;
    restore(mask);
}

void round_robin_print_stats(void) {
    intmask mask;
    
    mask = disable();
    
    kprintf("\n=== Round-Robin Scheduler Statistics ===\n");
    kprintf("Queue Length: %u (max: %u)\n", 
            rr_stats.current_queue_length, rr_stats.max_queue_length);
    kprintf("Total Processes: %u\n", rr_stats.total_processes);
    kprintf("Context Switches: %llu\n", rr_stats.total_context_switches);
    kprintf("Quantum Expirations: %llu\n", rr_stats.total_quantum_expires);
    kprintf("Current Quantum: %u ms\n", rr_quantum);
    
    restore(mask);
}

void round_robin_print_queue(void) {
    rr_node_t *node;
    int count = 0;
    intmask mask;
    
    mask = disable();
    
    kprintf("\n=== Round-Robin Queue ===\n");
    kprintf("Count: %u, Quantum: %u ms\n", rr_queue_count, rr_quantum);
    kprintf("PID   TimeLeft  TotalTime  Rounds\n");
    kprintf("----  --------  ---------  ------\n");
    
    if (rr_queue_head != NULL) {
        node = rr_queue_head;
        do {
            char marker = (node == rr_current) ? '*' : ' ';
            kprintf("%c%3d  %8u  %9llu  %6u\n",
                    marker, node->pid, node->time_remaining,
                    node->total_time, node->rounds);
            node = node->next;
            count++;
        } while (node != rr_queue_head && count < NPROC);
    }
    
    kprintf("\n");
    
    restore(mask);
}

bool round_robin_validate(void) {
    rr_node_t *node;
    int count = 0;
    bool valid = true;
    intmask mask;
    
    mask = disable();
    
    if (rr_queue_head == NULL) {
        if (rr_queue_count != 0) {
            kprintf("RR: Queue head NULL but count = %u\n", rr_queue_count);
            valid = false;
        }
        restore(mask);
        return valid;
    }
    
    node = rr_queue_head;
    do {
        count++;
        
        if (node->pid < 0 || node->pid >= NPROC) {
            kprintf("RR: Invalid PID %d in queue\n", node->pid);
            valid = false;
        }
        
        if (node->next == NULL || node->prev == NULL) {
            kprintf("RR: Broken links at PID %d\n", node->pid);
            valid = false;
            break;
        }
        
        if (node->next->prev != node) {
            kprintf("RR: Forward/backward link mismatch at PID %d\n", node->pid);
            valid = false;
        }
        
        node = node->next;
        
        if (count > NPROC) {
            kprintf("RR: Queue appears corrupted (too many nodes)\n");
            valid = false;
            break;
        }
    } while (node != rr_queue_head);
    
    if (count != rr_queue_count) {
        kprintf("RR: Count mismatch: %d traversed vs %u stored\n",
                count, rr_queue_count);
        valid = false;
    }
    
    restore(mask);
    
    return valid;
}
