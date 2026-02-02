#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scheduler.h"
#include "round_robin.h"
#include "priority.h"
#include "multilevel_queue.h"
#include "lottery.h"
#include "cfs.h"
#include "realtime.h"
#include "../include/kernel.h"
#include "../include/process.h"
#include "../include/interrupts.h"

scheduler_ops_t *current_scheduler = NULL;

uint32_t sched_policy = DEFAULT_SCHED_POLICY;

sched_stats_t sched_stats;

ready_queue_t ready_queue;

static ready_node_t node_pool[NPROC];
static ready_node_t *free_nodes = NULL;

static uint32_t current_quantum = DEFAULT_QUANTUM;

static uint32_t quantum_remaining = DEFAULT_QUANTUM;

static sched_proc_stats_t proc_stats[NPROC];

static sid32 sched_lock;

volatile bool need_resched = false;

static uint64_t system_ticks = 0;

static bool sched_initialized = false;

extern proc_t proctab[];
extern pid32 currpid;

static void node_pool_init(void) {
    int i;
    
    free_nodes = &node_pool[0];
    for (i = 0; i < NPROC - 1; i++) {
        node_pool[i].next = &node_pool[i + 1];
        node_pool[i].prev = NULL;
        node_pool[i].pid = -1;
    }
    node_pool[NPROC - 1].next = NULL;
}

static ready_node_t *node_alloc(void) {
    ready_node_t *node;
    
    if (free_nodes == NULL) {
        return NULL;
    }
    
    node = free_nodes;
    free_nodes = free_nodes->next;
    
    node->next = NULL;
    node->prev = NULL;
    node->pid = -1;
    node->priority = 0;
    node->time_slice = current_quantum;
    node->enqueue_time = 0;
    
    return node;
}

static void node_free(ready_node_t *node) {
    if (node == NULL) {
        return;
    }
    
    node->next = free_nodes;
    free_nodes = node;
}

void ready_queue_init(void) {
    ready_queue.head = NULL;
    ready_queue.tail = NULL;
    ready_queue.count = 0;
    ready_queue.priority = 0;
    
    node_pool_init();
}

void ready_enqueue(pid32 pid) {
    ready_node_t *node;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    
    node = node_alloc();
    if (node == NULL) {
        restore(mask);
        return;
    }
    
    node->pid = pid;
    node->priority = proctab[pid].pprio;
    node->time_slice = current_quantum;
    node->enqueue_time = system_ticks;
    
    node->next = NULL;
    node->prev = ready_queue.tail;
    
    if (ready_queue.tail != NULL) {
        ready_queue.tail->next = node;
    } else {
        ready_queue.head = node;
    }
    
    ready_queue.tail = node;
    ready_queue.count++;
    
    sched_stats.runnable_count++;
    if (sched_stats.runnable_count > sched_stats.max_runnable) {
        sched_stats.max_runnable = sched_stats.runnable_count;
    }
    
    restore(mask);
}

void ready_dequeue(pid32 pid) {
    ready_node_t *node;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    
    node = ready_queue.head;
    while (node != NULL && node->pid != pid) {
        node = node->next;
    }
    
    if (node == NULL) {
        restore(mask);
        return;
    }
    
    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        ready_queue.head = node->next;
    }
    
    if (node->next != NULL) {
        node->next->prev = node->prev;
    } else {
        ready_queue.tail = node->prev;
    }
    
    ready_queue.count--;
    sched_stats.runnable_count--;
    
    node_free(node);
    
    restore(mask);
}

pid32 ready_peek(void) {
    if (ready_queue.head == NULL) {
        return -1;
    }
    return ready_queue.head->pid;
}

pid32 ready_pop(void) {
    ready_node_t *node;
    pid32 pid;
    intmask mask;
    
    mask = disable();
    
    if (ready_queue.head == NULL) {
        restore(mask);
        return -1;
    }
    
    node = ready_queue.head;
    pid = node->pid;
    
    ready_queue.head = node->next;
    if (ready_queue.head != NULL) {
        ready_queue.head->prev = NULL;
    } else {
        ready_queue.tail = NULL;
    }
    
    ready_queue.count--;
    sched_stats.runnable_count--;
    
    node_free(node);
    
    restore(mask);
    
    return pid;
}

bool ready_queue_empty(void) {
    return ready_queue.head == NULL;
}

uint32_t ready_queue_count(void) {
    return ready_queue.count;
}

void scheduler_init(scheduler_type_t type) {
    int i;
    intmask mask;
    
    mask = disable();
    
    ready_queue_init();
    
    memset(&sched_stats, 0, sizeof(sched_stats));
    for (i = 0; i < NPROC; i++) {
        memset(&proc_stats[i], 0, sizeof(sched_proc_stats_t));
    }
    
    sched_lock = semcreate(1);
    
    sched_policy = type;
    
    switch (type) {
        case SCHEDULER_ROUND_ROBIN:
            round_robin_init();
            current_scheduler = round_robin_get_ops();
            break;
            
        case SCHEDULER_PRIORITY:
            priority_init();
            current_scheduler = priority_get_ops();
            break;
            
        case SCHEDULER_MLFQ:
            mlfq_init();
            current_scheduler = mlfq_get_ops();
            break;
            
        case SCHEDULER_LOTTERY:
            lottery_init();
            current_scheduler = lottery_get_ops();
            break;
            
        case SCHEDULER_CFS:
            cfs_init();
            current_scheduler = cfs_get_ops();
            break;
            
        case SCHEDULER_EDF:
            realtime_init();
            current_scheduler = realtime_get_ops();
            break;
            
        default:

            priority_init();
            current_scheduler = priority_get_ops();
            sched_policy = SCHEDULER_PRIORITY;
            break;
    }
    
    sched_initialized = true;
    
    restore(mask);
    
    kprintf("Scheduler initialized: %s\n", current_scheduler->name);
}

void scheduler_shutdown(void) {
    intmask mask;
    
    mask = disable();
    
    if (current_scheduler != NULL && current_scheduler->shutdown != NULL) {
        current_scheduler->shutdown();
    }
    
    sched_initialized = false;
    
    restore(mask);
}

syscall scheduler_switch(scheduler_type_t type) {
    intmask mask;
    
    mask = disable();
    
    if (current_scheduler != NULL && current_scheduler->shutdown != NULL) {
        current_scheduler->shutdown();
    }
    
    switch (type) {
        case SCHEDULER_ROUND_ROBIN:
            round_robin_init();
            current_scheduler = round_robin_get_ops();
            break;
            
        case SCHEDULER_PRIORITY:
            priority_init();
            current_scheduler = priority_get_ops();
            break;
            
        case SCHEDULER_MLFQ:
            mlfq_init();
            current_scheduler = mlfq_get_ops();
            break;
            
        case SCHEDULER_LOTTERY:
            lottery_init();
            current_scheduler = lottery_get_ops();
            break;
            
        case SCHEDULER_CFS:
            cfs_init();
            current_scheduler = cfs_get_ops();
            break;
            
        case SCHEDULER_EDF:
            realtime_init();
            current_scheduler = realtime_get_ops();
            break;
            
        default:
            restore(mask);
            return SYSERR;
    }
    
    sched_policy = type;
    
    restore(mask);
    
    kprintf("Scheduler switched to: %s\n", current_scheduler->name);
    
    return OK;
}

void schedule(void) {
    intmask mask;
    
    if (!sched_initialized || current_scheduler == NULL) {
        return;
    }
    
    mask = disable();
    
    sched_stats.total_schedules++;
    need_resched = false;
    
    if (current_scheduler->schedule != NULL) {
        current_scheduler->schedule();
    }
    
    restore(mask);
}

void resched(void) {
    intmask mask;
    
    mask = disable();
    
    need_resched = true;
    
    schedule();
    
    restore(mask);
}

void yield(void) {
    intmask mask;
    
    mask = disable();
    
    sched_stats.voluntary_yields++;
    proc_stats[currpid].voluntary_switches++;
    
    if (current_scheduler != NULL && current_scheduler->yield != NULL) {
        current_scheduler->yield();
    } else {

        if (proctab[currpid].pstate == PR_CURR) {
            proctab[currpid].pstate = PR_READY;
            ready_enqueue(currpid);
        }
        resched();
    }
    
    restore(mask);
}

void preempt(void) {
    intmask mask;
    
    mask = disable();
    
    sched_stats.preemptions++;
    proc_stats[currpid].involuntary_switches++;
    
    if (current_scheduler != NULL && current_scheduler->preempt != NULL) {
        current_scheduler->preempt();
    } else {

        if (proctab[currpid].pstate == PR_CURR) {
            proctab[currpid].pstate = PR_READY;
            ready_enqueue(currpid);
        }
        resched();
    }
    
    restore(mask);
}

syscall setpriority(pid32 pid, uint32_t priority) {
    uint32_t old_priority;
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (proctab[pid].pstate == PR_FREE) {
        restore(mask);
        return SYSERR;
    }
    
    if (priority > PRIORITY_MAX) {
        priority = PRIORITY_MAX;
    }
    
    old_priority = proctab[pid].pprio;
    
    if (current_scheduler != NULL && current_scheduler->set_priority != NULL) {
        current_scheduler->set_priority(pid, priority);
    } else {
        proctab[pid].pprio = priority;
    }
    
    if (proctab[pid].pstate == PR_READY) {
        resched();
    }
    
    restore(mask);
    
    return old_priority;
}

syscall getpriority(pid32 pid) {
    intmask mask;
    uint32_t priority;
    
    if (pid < 0 || pid >= NPROC) {
        return SYSERR;
    }
    
    mask = disable();
    
    if (proctab[pid].pstate == PR_FREE) {
        restore(mask);
        return SYSERR;
    }
    
    if (current_scheduler != NULL && current_scheduler->get_priority != NULL) {
        priority = current_scheduler->get_priority(pid);
    } else {
        priority = proctab[pid].pprio;
    }
    
    restore(mask);
    
    return priority;
}

syscall nice(int32_t increment) {
    int32_t new_priority;
    intmask mask;
    
    mask = disable();
    
    new_priority = (int32_t)proctab[currpid].pprio - increment;
    
    if (new_priority < PRIORITY_MIN) {
        new_priority = PRIORITY_MIN;
    }
    if (new_priority > PRIORITY_MAX) {
        new_priority = PRIORITY_MAX;
    }
    
    proctab[currpid].pprio = new_priority;
    
    restore(mask);
    
    return new_priority;
}

void sched_set_quantum(uint32_t quantum) {
    if (quantum < MIN_QUANTUM) {
        quantum = MIN_QUANTUM;
    }
    if (quantum > MAX_QUANTUM) {
        quantum = MAX_QUANTUM;
    }
    
    current_quantum = quantum;
    
    if (current_scheduler != NULL && current_scheduler->set_quantum != NULL) {
        current_scheduler->set_quantum(quantum);
    }
}

uint32_t sched_get_quantum(void) {
    if (current_scheduler != NULL && current_scheduler->get_quantum != NULL) {
        return current_scheduler->get_quantum();
    }
    return current_quantum;
}

void sched_tick(void) {
    intmask mask;
    
    mask = disable();
    
    system_ticks++;
    
    if (currpid >= 0 && currpid < NPROC) {
        proc_stats[currpid].total_runtime++;
        proc_stats[currpid].last_runtime++;
    }
    
    if (current_scheduler != NULL && current_scheduler->tick != NULL) {
        current_scheduler->tick();
    } else {

        if (quantum_remaining > 0) {
            quantum_remaining--;
        }
        
        if (quantum_remaining == 0) {
            sched_stats.quantum_expirations++;
            quantum_remaining = current_quantum;
            need_resched = true;
        }
    }
    
    restore(mask);
}

uint64_t sched_get_time(void) {
    return system_ticks;
}

void sched_ready(pid32 pid) {
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    
    if (current_scheduler != NULL && current_scheduler->enqueue != NULL) {
        current_scheduler->enqueue(pid);
    } else {
        ready_enqueue(pid);
    }
    
    restore(mask);
}

void sched_block(pid32 pid) {
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    
    sched_stats.blocked_count++;
    
    if (current_scheduler != NULL && current_scheduler->dequeue != NULL) {
        current_scheduler->dequeue(pid);
    } else {
        ready_dequeue(pid);
    }
    
    if (pid == currpid) {
        resched();
    }
    
    restore(mask);
}

void sched_wakeup(pid32 pid) {
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    
    sched_stats.blocked_count--;
    
    proctab[pid].pstate = PR_READY;
    
    if (current_scheduler != NULL && current_scheduler->enqueue != NULL) {
        current_scheduler->enqueue(pid);
    } else {
        ready_enqueue(pid);
    }
    
    if (proctab[pid].pprio > proctab[currpid].pprio) {
        need_resched = true;
    }
    
    restore(mask);
}

void sched_new_process(pid32 pid) {
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    
    memset(&proc_stats[pid], 0, sizeof(sched_proc_stats_t));
    
    restore(mask);
}

void sched_exit(pid32 pid) {
    intmask mask;
    
    if (pid < 0 || pid >= NPROC) {
        return;
    }
    
    mask = disable();
    
    if (current_scheduler != NULL && current_scheduler->dequeue != NULL) {
        current_scheduler->dequeue(pid);
    } else {
        ready_dequeue(pid);
    }
    
    if (pid == currpid) {
        resched();
    }
    
    restore(mask);
}

void sched_get_stats(sched_stats_t *stats) {
    intmask mask;
    
    if (stats == NULL) {
        return;
    }
    
    mask = disable();
    
    if (current_scheduler != NULL && current_scheduler->get_stats != NULL) {
        current_scheduler->get_stats(stats);
    } else {
        memcpy(stats, &sched_stats, sizeof(sched_stats_t));
    }
    
    restore(mask);
}

void sched_get_proc_stats(pid32 pid, sched_proc_stats_t *stats) {
    intmask mask;
    
    if (pid < 0 || pid >= NPROC || stats == NULL) {
        return;
    }
    
    mask = disable();
    memcpy(stats, &proc_stats[pid], sizeof(sched_proc_stats_t));
    restore(mask);
}

void sched_reset_stats(void) {
    int i;
    intmask mask;
    
    mask = disable();
    
    if (current_scheduler != NULL && current_scheduler->reset_stats != NULL) {
        current_scheduler->reset_stats();
    }
    
    memset(&sched_stats, 0, sizeof(sched_stats));
    for (i = 0; i < NPROC; i++) {
        memset(&proc_stats[i], 0, sizeof(sched_proc_stats_t));
    }
    
    restore(mask);
}

void sched_print_stats(void) {
    intmask mask;
    
    mask = disable();
    
    kprintf("\n=== Scheduler Statistics ===\n");
    kprintf("Scheduler: %s\n", current_scheduler ? current_scheduler->name : "None");
    kprintf("Total Schedules: %llu\n", sched_stats.total_schedules);
    kprintf("Context Switches: %llu\n", sched_stats.context_switches);
    kprintf("Preemptions: %u\n", sched_stats.preemptions);
    kprintf("Voluntary Yields: %u\n", sched_stats.voluntary_yields);
    kprintf("Quantum Expirations: %llu\n", sched_stats.quantum_expirations);
    kprintf("Runnable: %u\n", sched_stats.runnable_count);
    kprintf("Blocked: %u\n", sched_stats.blocked_count);
    kprintf("Max Runnable: %u\n", sched_stats.max_runnable);
    
    if (current_scheduler != NULL && current_scheduler->print_stats != NULL) {
        current_scheduler->print_stats();
    }
    
    kprintf("\n");
    
    restore(mask);
}

void sched_print_ready_queue(void) {
    ready_node_t *node;
    intmask mask;
    
    mask = disable();
    
    kprintf("\n=== Ready Queue ===\n");
    kprintf("Count: %u\n", ready_queue.count);
    kprintf("PID   Priority  TimeSlice  EnqueueTime\n");
    kprintf("----  --------  ---------  -----------\n");
    
    node = ready_queue.head;
    while (node != NULL) {
        kprintf("%4d  %8u  %9u  %11llu\n",
                node->pid, node->priority, node->time_slice, node->enqueue_time);
        node = node->next;
    }
    
    kprintf("\n");
    
    restore(mask);
}

bool sched_validate(void) {
    ready_node_t *node;
    uint32_t count = 0;
    intmask mask;
    bool valid = true;
    
    mask = disable();
    
    node = ready_queue.head;
    while (node != NULL) {
        count++;
        
        if (node->pid < 0 || node->pid >= NPROC) {
            kprintf("Invalid PID in ready queue: %d\n", node->pid);
            valid = false;
        }
        
        if (proctab[node->pid].pstate != PR_READY) {
            kprintf("Process %d in ready queue but state is %d\n",
                    node->pid, proctab[node->pid].pstate);
            valid = false;
        }
        
        node = node->next;
        
        if (count > NPROC) {
            kprintf("Ready queue appears circular!\n");
            valid = false;
            break;
        }
    }
    
    if (count != ready_queue.count) {
        kprintf("Ready queue count mismatch: %u vs %u\n", 
                count, ready_queue.count);
        valid = false;
    }
    
    restore(mask);
    
    return valid;
}

void sched_dump(void) {
    int i;
    intmask mask;
    
    mask = disable();
    
    kprintf("\n=== Scheduler State Dump ===\n");
    kprintf("Current PID: %d\n", currpid);
    kprintf("Need Resched: %s\n", need_resched ? "Yes" : "No");
    kprintf("Quantum: %u ms\n", current_quantum);
    kprintf("Quantum Remaining: %u\n", quantum_remaining);
    kprintf("System Ticks: %llu\n", system_ticks);
    
    sched_print_ready_queue();
    
    kprintf("\n=== Per-Process Stats ===\n");
    kprintf("PID   State   Priority  Runtime    Switches\n");
    kprintf("----  ------  --------  ---------  --------\n");
    
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
            
            kprintf("%4d  %6s  %8u  %9llu  %8u\n",
                    i, state, proctab[i].pprio,
                    proc_stats[i].total_runtime,
                    proc_stats[i].context_switches);
        }
    }
    
    kprintf("\n");
    
    restore(mask);
}

const char *sched_get_name(void) {
    if (current_scheduler != NULL) {
        return current_scheduler->name;
    }
    return "None";
}
