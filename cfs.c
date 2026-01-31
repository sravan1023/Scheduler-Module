#include "cfs.h"
#include "../include/kernel.h"
#include "../include/process.h"
#include <stdlib.h>
#include <string.h>

/* Weight values for nice levels */
static const uint32_t cfs_weight_table[CFS_NICE_LEVELS] = {
    88761, 71755, 56483, 46273, 36291,
    29154, 23254, 18705, 14949, 11916,
    9548,  7620,  6100,  4904,  3906,
    3121,  2501,  1991,  1586,  1277,
    1024,  820,   655,   526,   423,
    335,   272,   215,   172,   137,
    110,   87,    70,    56,    45,
    36,    29,    23,    18,    15,
};

/* Weight multipliers for inverse calculations */
static const uint32_t cfs_wmult_table[CFS_NICE_LEVELS] = {
    48388, 59856, 76040, 92818, 118348,
    147320, 184698, 229616, 287308, 360437,
    449829, 563644, 704093, 875809, 1099582,
    1376151, 1717300, 2157191, 2708050, 3363326,
    4194304, 5237765, 6557202, 8165337, 10153587,
    12820798, 15790321, 19976592, 24970740, 31350126,
    39045157, 49367440, 61356676, 76695844, 95443717,
    119304647, 148102320, 186737708, 238609294, 286331153,
};

/* Run queue containing all runnable tasks */
static cfs_rq_t cfs_rq;

/* Pre-allocated task pool and free list */
#define CFS_MAX_TASKS 256
static cfs_task_t task_pool[CFS_MAX_TASKS];
static cfs_task_t *free_tasks = NULL;

static cfs_stats_t stats;
static scheduler_ops_t cfs_ops;
static uint64_t system_clock = 0;

static cfs_task_t *alloc_task(void);
static void free_task(cfs_task_t *task);
static cfs_task_t *find_task(pid32 pid);
static void insert_task(cfs_task_t *task);
static void remove_task(cfs_task_t *task);
static void update_current(void);
static uint64_t max64(uint64_t a, uint64_t b);

static uint64_t max64(uint64_t a, uint64_t b)
{
    return (a > b) ? a : b;
}

/* Allocate a task from the free pool */
static cfs_task_t *alloc_task(void)
{
    if (free_tasks == NULL) {
        return NULL;
    }
    
    cfs_task_t *task = free_tasks;
    free_tasks = free_tasks->next;
    
    memset(task, 0, sizeof(cfs_task_t));
    return task;
}

static void free_task(cfs_task_t *task)
{
    if (task == NULL) {
        return;
    }
    
    task->next = free_tasks;
    free_tasks = task;
}

static cfs_task_t *find_task(pid32 pid)
{
    cfs_task_t *task = cfs_rq.tasks_timeline;
    
    while (task != NULL) {
        if (task->pid == pid) {
            return task;
        }
        task = task->next;
    }
    
    return NULL;
}

/* Insert task into timeline sorted by vruntime */
static void insert_task(cfs_task_t *task)
{
    task->on_rq = true;
    
    if (cfs_rq.tasks_timeline == NULL) {
        task->prev = NULL;
        task->next = NULL;
        cfs_rq.tasks_timeline = task;
        cfs_rq.leftmost = task;
        return;
    }
    
    /* Find insertion point in sorted list */
    cfs_task_t *curr = cfs_rq.tasks_timeline;
    cfs_task_t *prev = NULL;
    
    while (curr != NULL && curr->vruntime <= task->vruntime) {
        prev = curr;
        curr = curr->next;
    }
    
    if (prev == NULL) {
        /* Insert at head */
        task->prev = NULL;
        task->next = cfs_rq.tasks_timeline;
        cfs_rq.tasks_timeline->prev = task;
        cfs_rq.tasks_timeline = task;
        cfs_rq.leftmost = task;
    } else {
        /* Insert in middle or end */
        task->prev = prev;
        task->next = prev->next;
        if (prev->next != NULL) {
            prev->next->prev = task;
        }
        prev->next = task;
    }
}

static void remove_task(cfs_task_t *task)
{
    if (!task->on_rq) {
        return;
    }
    
    task->on_rq = false;
    
    if (cfs_rq.tasks_timeline == task) {
        cfs_rq.tasks_timeline = task->next;
    }
    if (cfs_rq.leftmost == task) {
        cfs_rq.leftmost = task->next;
    }
    
    if (task->prev != NULL) {
        task->prev->next = task->next;
    }
    if (task->next != NULL) {
        task->next->prev = task->prev;
    }
    
    task->prev = NULL;
    task->next = NULL;
}

/* Initialize the CFS scheduler */
void cfs_init(void)
{
    /* Build free list from task pool */
    free_tasks = NULL;
    for (int i = CFS_MAX_TASKS - 1; i >= 0; i--) {
        task_pool[i].next = free_tasks;
        free_tasks = &task_pool[i];
    }
    
    memset(&cfs_rq, 0, sizeof(cfs_rq));
    cfs_rq.min_vruntime = 0;
    
    memset(&stats, 0, sizeof(stats));
    
    system_clock = 0;
    
    cfs_ops.init = cfs_init;
    cfs_ops.shutdown = cfs_shutdown;
    cfs_ops.schedule = cfs_schedule;
    cfs_ops.yield = cfs_yield;
    cfs_ops.preempt = cfs_preempt;
    cfs_ops.enqueue = cfs_enqueue;
    cfs_ops.dequeue = cfs_dequeue;
    cfs_ops.tick = cfs_tick;
    cfs_ops.get_stats = (void (*)(void *))cfs_get_stats;
    cfs_ops.print_stats = cfs_print_stats;
    cfs_ops.type = SCHED_CFS;
    cfs_ops.name = "cfs";
}

void cfs_shutdown(void)
{

    cfs_task_t *task = cfs_rq.tasks_timeline;
    while (task != NULL) {
        cfs_task_t *next = task->next;
        free_task(task);
        task = next;
    }
    
    memset(&cfs_rq, 0, sizeof(cfs_rq));
}

scheduler_ops_t *cfs_get_ops(void)
{
    return &cfs_ops;
}

/* Convert nice value to weight  */
uint32_t cfs_nice_to_weight(int nice)
{
    int index = nice + 20;
    if (index < 0) index = 0;
    if (index >= CFS_NICE_LEVELS) index = CFS_NICE_LEVELS - 1;
    return cfs_weight_table[index];
}

uint32_t cfs_nice_to_wmult(int nice)
{
    int index = nice + 20;
    if (index < 0) index = 0;
    if (index >= CFS_NICE_LEVELS) index = CFS_NICE_LEVELS - 1;
    return cfs_wmult_table[index];
}

int cfs_set_nice(pid32 pid, int nice)
{
    cfs_task_t *task = find_task(pid);
    if (task == NULL) {
        return 0;
    }
    
    if (nice < CFS_NICE_MIN) nice = CFS_NICE_MIN;
    if (nice > CFS_NICE_MAX) nice = CFS_NICE_MAX;
    
    int old_nice = task->nice;
    uint32_t old_weight = task->weight;
    
    task->nice = nice;
    task->weight = cfs_nice_to_weight(nice);
    
    cfs_rq.load_weight = cfs_rq.load_weight - old_weight + task->weight;
    
    if (task->on_rq && task != cfs_rq.curr) {
        remove_task(task);
        insert_task(task);
    }
    
    return old_nice;
}

int cfs_get_nice(pid32 pid)
{
    cfs_task_t *task = find_task(pid);
    if (task == NULL) {
        return 0;
    }
    return task->nice;
}

/* Convert real time to virtual time based on task weight */
uint64_t cfs_calc_delta(uint64_t delta_exec, uint32_t weight)
{
    if (weight == 0) {
        return delta_exec;
    }
    
    uint64_t delta_vruntime = (delta_exec * CFS_WEIGHT_NICE0) / weight;
    return delta_vruntime;
}

/* Update task's virtual runtime based on real execution time */
void cfs_update_vruntime(cfs_task_t *task, uint64_t delta_exec)
{
    if (task == NULL) {
        return;
    }
    
    uint64_t delta_vruntime = cfs_calc_delta(delta_exec, task->weight);
    
    task->vruntime += delta_vruntime;
    task->sum_exec += delta_exec;
    
    stats.total_runtime += delta_exec;
}

/* Update min_vruntime to track smallest vruntime in the system */
void cfs_update_min_vruntime(void)
{
    uint64_t vruntime = cfs_rq.min_vruntime;
    
    if (cfs_rq.curr != NULL) {
        vruntime = cfs_rq.curr->vruntime;
    }
    
    if (cfs_rq.leftmost != NULL) {
        if (cfs_rq.curr == NULL) {
            vruntime = cfs_rq.leftmost->vruntime;
        } else {
            if (cfs_rq.leftmost->vruntime < vruntime) {
                vruntime = cfs_rq.leftmost->vruntime;
            }
        }
    }
    
    /* min_vruntime only moves forward */
    cfs_rq.min_vruntime = max64(cfs_rq.min_vruntime, vruntime);
}

/* Set initial vruntime for a task (prevents new tasks from starving existing ones) */
void cfs_place_task(cfs_task_t *task, bool initial)
{
    uint64_t vruntime = cfs_rq.min_vruntime;
    
    if (initial) {
        /* New tasks start with penalty to prevent gaming the system */
        uint32_t latency = cfs_sched_latency();
        vruntime += cfs_calc_delta(latency / 2, task->weight);
    }
    
    task->vruntime = max64(task->vruntime, vruntime);
}

/* Calculate scheduling latency */
uint32_t cfs_sched_latency(void)
{
    uint32_t latency = CFS_TARGET_LATENCY;
    
    if (cfs_rq.nr_running > 8) {
        latency = CFS_MIN_GRANULARITY * cfs_rq.nr_running;
    }
    
    return latency;
}

/* Calculate task's timeslice proportional to its weight */
uint32_t cfs_timeslice(cfs_task_t *task)
{
    if (cfs_rq.nr_running == 0 || task == NULL) {
        return CFS_TARGET_LATENCY;
    }
    
    uint32_t latency = cfs_sched_latency();
    
    uint32_t slice = (latency * task->weight) / cfs_rq.load_weight;
    
    if (slice < CFS_MIN_GRANULARITY) {
        slice = CFS_MIN_GRANULARITY;
    }
    
    return slice;
}

static void update_current(void)
{
    cfs_task_t *curr = cfs_rq.curr;
    if (curr == NULL) {
        return;
    }
    
    uint64_t now = cfs_rq.clock_task;
    uint64_t delta_exec = now - curr->exec_start;
    
    if (delta_exec == 0) {
        return;
    }
    
    cfs_update_vruntime(curr, delta_exec);
    
    curr->exec_start = now;
    
    cfs_update_min_vruntime();
}

/* Pick next task to run*/
cfs_task_t *cfs_pick_next_task(void)
{
    return cfs_rq.leftmost;
}

/* Check if current task should be preempted by leftmost task */
bool cfs_check_preempt(void)
{
    cfs_task_t *curr = cfs_rq.curr;
    cfs_task_t *next = cfs_rq.leftmost;
    
    if (curr == NULL || next == NULL) {
        return curr == NULL && next != NULL;
    }
    
    /* Preempt if vruntime difference exceeds granularity */
    uint64_t gran = cfs_calc_delta(CFS_MIN_GRANULARITY, curr->weight);
    
    return (next->vruntime + gran < curr->vruntime);
}

void cfs_put_prev_task(void)
{
    cfs_task_t *prev = cfs_rq.curr;
    if (prev == NULL) {
        return;
    }
    
    update_current();
    
    if (prev->on_rq) {
        remove_task(prev);
        insert_task(prev);
    }
    
    cfs_rq.curr = NULL;
}

void cfs_set_curr_task(cfs_task_t *task)
{
    task->exec_start = cfs_rq.clock_task;
    cfs_rq.curr = task;
}

/* Main scheduling decision: pick and switch to next task */
void cfs_schedule(void)
{
    /* Update clocks */
    cfs_rq.clock = system_clock;
    cfs_rq.clock_task = system_clock;
    
    if (cfs_rq.curr != NULL) {
        update_current();
    }
    
    cfs_put_prev_task();
    
    cfs_task_t *next = cfs_pick_next_task();
    
    if (next == NULL) {
        /* Idle: no tasks to run */
        return;
    }
    
    remove_task(next);
    
    cfs_set_curr_task(next);
    
    pid32 old_pid = (cfs_rq.curr != NULL) ? cfs_rq.curr->pid : -1;
    
    if (old_pid != next->pid) {
        stats.switches++;
        
        extern void context_switch(pid32 old, pid32 new);
        context_switch(old_pid, next->pid);
    }
}

void cfs_yield(void)
{
    cfs_task_t *curr = cfs_rq.curr;
    if (curr == NULL) {
        return;
    }
    
    update_current();
    
    if (cfs_rq.leftmost != NULL) {
        curr->vruntime = max64(curr->vruntime, cfs_rq.leftmost->vruntime);
    }
    
    cfs_schedule();
}

void cfs_preempt(void)
{
    cfs_schedule();
}

/* Add a task to the run queue */
void cfs_enqueue(pid32 pid)
{
    cfs_task_t *task = find_task(pid);
    
    if (task == NULL) {
        /* New task: allocate and initialize */
        task = alloc_task();
        if (task == NULL) {
            return;
        }
        
        task->pid = pid;
        task->nice = CFS_NICE_DEFAULT;
        task->weight = cfs_nice_to_weight(CFS_NICE_DEFAULT);
        task->vruntime = cfs_rq.min_vruntime;
        task->sum_exec = 0;
        
        cfs_place_task(task, true);
    } else if (task->on_rq) {
        return;
    } else {
        cfs_place_task(task, false);
    }
    
    insert_task(task);
    
    cfs_rq.nr_running++;
    cfs_rq.load_weight += task->weight;
    
    if (cfs_check_preempt()) {
    }
}

/* Remove a task from the run queue permanently */
void cfs_dequeue(pid32 pid)
{
    cfs_task_t *task = find_task(pid);
    if (task == NULL) {
        return;
    }
    
    if (task == cfs_rq.curr) {
        update_current();
        cfs_rq.curr = NULL;
    }
    
    if (task->on_rq) {
        remove_task(task);
        cfs_rq.nr_running--;
        cfs_rq.load_weight -= task->weight;
    }
    
    free_task(task);
    
    cfs_update_min_vruntime();
}

void cfs_sleep(pid32 pid)
{
    cfs_task_t *task = find_task(pid);
    if (task == NULL) {
        return;
    }
    
    task->sleep_start = system_clock;
    
    if (task == cfs_rq.curr) {
        update_current();
        cfs_rq.curr = NULL;
    }
    
    if (task->on_rq) {
        remove_task(task);
        cfs_rq.nr_running--;
        cfs_rq.load_weight -= task->weight;
    }
}

/* Wake up a sleeping task and re-add to run queue */
void cfs_wakeup(pid32 pid)
{
    cfs_task_t *task = find_task(pid);
    if (task == NULL || task->on_rq) {
        return;
    }
    
    uint64_t sleep_time = system_clock - task->sleep_start;
    stats.sleep_time += sleep_time;
    
    /* Award vruntime credit to tasks that slept (favor interactive tasks) */
    if (CFS_SLEEPER_BONUS && sleep_time > 0) {
        uint64_t credit = cfs_sleeper_credit(task, sleep_time);
        if (task->vruntime > credit) {
            task->vruntime -= credit;
        }
    }
    
    cfs_place_task(task, false);
    
    insert_task(task);
    cfs_rq.nr_running++;
    cfs_rq.load_weight += task->weight;
    
    if (cfs_check_preempt()) {
        /* Could trigger reschedule for interactive task */
    }
}

/* Calculate vruntime credit for sleeping tasks (capped to prevent abuse) */
uint64_t cfs_sleeper_credit(cfs_task_t *task, uint64_t sleep_time)
{
    /* Cap credit at half the scheduling latency */
    uint64_t max_credit = cfs_calc_delta(cfs_sched_latency() / 2, task->weight);
    
    uint64_t credit = cfs_calc_delta(sleep_time, task->weight) / 2;
    
    if (credit > max_credit) {
        credit = max_credit;
    }
    
    return credit;
}

/* Timer tick: update runtime and check if current task exhausted timeslice */
void cfs_tick(void)
{
    system_clock++;
    
    cfs_rq.clock = system_clock;
    cfs_rq.clock_task = system_clock;
    
    cfs_task_t *curr = cfs_rq.curr;
    if (curr == NULL) {
        return;
    }
    
    update_current();
    
    uint64_t ideal_runtime = cfs_timeslice(curr);
    uint64_t actual_runtime = curr->sum_exec - curr->prev_sum_exec;
    
    if (actual_runtime >= ideal_runtime) {
        /* Task exhausted its timeslice */
        if (cfs_rq.nr_running > 1) {
            /* Reschedule if other tasks waiting */
            curr->prev_sum_exec = curr->sum_exec;
            cfs_schedule();
        }
    }
}

void cfs_update_clock(uint64_t delta)
{
    cfs_rq.clock += delta;
    cfs_rq.clock_task += delta;
}

void cfs_get_stats(cfs_stats_t *s)
{
    if (s == NULL) {
        return;
    }
    
    s->switches = stats.switches;
    s->total_runtime = stats.total_runtime;
    s->wait_time = stats.wait_time;
    s->sleep_time = stats.sleep_time;
    s->nr_migrations = stats.nr_migrations;
    s->fairness_index = stats.fairness_index;
}

void cfs_reset_stats(void)
{
    stats.switches = 0;
    stats.total_runtime = 0;
    stats.wait_time = 0;
    stats.sleep_time = 0;
    stats.nr_migrations = 0;
    
    cfs_task_t *task = cfs_rq.tasks_timeline;
    while (task != NULL) {
        task->sum_exec = 0;
        task->prev_sum_exec = 0;
        task = task->next;
    }
}

void cfs_print_stats(void)
{
    kprintf("\n=== CFS Scheduler Statistics ===\n");
    kprintf("Context switches: %llu\n", stats.switches);
    kprintf("Total runtime: %llu ticks\n", stats.total_runtime);
    kprintf("Total sleep time: %llu ticks\n", stats.sleep_time);
    kprintf("Tasks running: %u\n", cfs_rq.nr_running);
    kprintf("Total load weight: %u\n", cfs_rq.load_weight);
    kprintf("Min vruntime: %llu\n", cfs_rq.min_vruntime);
    kprintf("Target latency: %u ticks\n", cfs_sched_latency());
    
    kprintf("\nPer-task statistics:\n");
    cfs_task_t *task = cfs_rq.tasks_timeline;
    while (task != NULL) {
        kprintf("  PID %d: nice=%d, weight=%u, vruntime=%llu, exec=%llu\n",
                task->pid, task->nice, task->weight,
                task->vruntime, task->sum_exec);
        task = task->next;
    }
    
    if (cfs_rq.curr != NULL) {
        kprintf("\nCurrently running: PID %d\n", cfs_rq.curr->pid);
    }
}

void cfs_print_rq(void)
{
    kprintf("\n=== CFS Run Queue ===\n");
    kprintf("nr_running: %u\n", cfs_rq.nr_running);
    kprintf("load_weight: %u\n", cfs_rq.load_weight);
    kprintf("min_vruntime: %llu\n", cfs_rq.min_vruntime);
    kprintf("clock: %llu\n", cfs_rq.clock);
    
    kprintf("\nTimeline (sorted by vruntime):\n");
    cfs_task_t *task = cfs_rq.tasks_timeline;
    int idx = 0;
    while (task != NULL) {
        char marker = (task == cfs_rq.leftmost) ? '*' : ' ';
        kprintf("  %c[%d] PID %d: vruntime=%llu, weight=%u\n",
                marker, idx++, task->pid, task->vruntime, task->weight);
        task = task->next;
    }
    
    if (cfs_rq.curr != NULL) {
        kprintf("\nCurrent: PID %d (vruntime=%llu)\n",
                cfs_rq.curr->pid, cfs_rq.curr->vruntime);
    }
}

void cfs_print_task(cfs_task_t *task)
{
    if (task == NULL) {
        kprintf("Task: NULL\n");
        return;
    }
    
    kprintf("Task PID %d:\n", task->pid);
    kprintf("  nice: %d\n", task->nice);
    kprintf("  weight: %u\n", task->weight);
    kprintf("  vruntime: %llu\n", task->vruntime);
    kprintf("  sum_exec: %llu\n", task->sum_exec);
    kprintf("  on_rq: %s\n", task->on_rq ? "yes" : "no");
    kprintf("  time_slice: %u\n", cfs_timeslice(task));
}

/* Validate run queue invariants */
bool cfs_validate(void)
{
    bool valid = true;
    uint32_t counted = 0;
    uint32_t counted_weight = 0;
    uint64_t prev_vruntime = 0;
    
    /* Check timeline is sorted and all tasks are marked on_rq */
    cfs_task_t *task = cfs_rq.tasks_timeline;
    while (task != NULL) {
        counted++;
        counted_weight += task->weight;
        
        if (task->vruntime < prev_vruntime) {
            kprintf("CFS validation: timeline not sorted at PID %d\n", task->pid);
            valid = false;
        }
        prev_vruntime = task->vruntime;
        
        if (!task->on_rq) {
            kprintf("CFS validation: task PID %d in timeline but not on_rq\n",
                    task->pid);
            valid = false;
        }
        
        task = task->next;
    }
    
    /* Verify counters match actual state */
    if (counted != cfs_rq.nr_running) {
        kprintf("CFS validation: nr_running mismatch (counted=%u, stored=%u)\n",
                counted, cfs_rq.nr_running);
        valid = false;
    }
    
    if (counted_weight != cfs_rq.load_weight) {
        kprintf("CFS validation: load_weight mismatch (counted=%u, stored=%u)\n",
                counted_weight, cfs_rq.load_weight);
        valid = false;
    }
    
    if (cfs_rq.tasks_timeline != cfs_rq.leftmost) {
        kprintf("CFS validation: leftmost doesn't match head\n");
        valid = false;
    }
    
    return valid;
}

cfs_task_t *cfs_get_task(pid32 pid)
{
    return find_task(pid);
}
