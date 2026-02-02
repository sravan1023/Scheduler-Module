#include "realtime.h"
#include "../include/kernel.h"
#include "../include/process.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static rt_task_t task_pool[RT_MAX_TASKS];
static rt_task_t *free_tasks = NULL;

static rt_task_t *ready_queue = NULL;

static rt_task_t *current_task = NULL;

static uint32_t task_count = 0;

static rt_algorithm_t current_algo = RT_DEFAULT_ALGO;

static uint64_t system_time = 0;

static rt_stats_t stats;

static scheduler_ops_t realtime_ops;

static rt_task_t *all_tasks = NULL;

static rt_task_t *alloc_task(void);
static void free_task(rt_task_t *task);
static rt_task_t *find_task(pid32 pid);
static void insert_ready(rt_task_t *task);
static void remove_ready(rt_task_t *task);
static void update_stats_completion(rt_task_t *task);

static rt_task_t *alloc_task(void)
{
    if (free_tasks == NULL) {
        return NULL;
    }
    
    rt_task_t *task = free_tasks;
    free_tasks = free_tasks->next;
    
    memset(task, 0, sizeof(rt_task_t));
    return task;
}

static void free_task(rt_task_t *task)
{
    if (task == NULL) {
        return;
    }
    
    task->next = free_tasks;
    free_tasks = task;
}

static rt_task_t *find_task(pid32 pid)
{
    rt_task_t *task = all_tasks;
    
    while (task != NULL) {
        if (task->pid == pid) {
            return task;
        }
        task = task->next;
    }
    
    return NULL;
}

static void insert_ready(rt_task_t *task)
{
    if (task == NULL) {
        return;
    }
    
    task->state = RT_STATE_READY;
    
    if (ready_queue == NULL) {
        task->next = NULL;
        ready_queue = task;
        return;
    }
    
    rt_task_t *prev = NULL;
    rt_task_t *curr = ready_queue;
    
    while (curr != NULL) {
        bool insert_here = false;
        
        switch (current_algo) {
        case RT_ALGO_EDF:

            insert_here = (task->absolute_deadline < curr->absolute_deadline);
            break;
            
        case RT_ALGO_RMS:
        case RT_ALGO_DMS:

            insert_here = (task->rms_priority > curr->rms_priority);
            break;
            
        case RT_ALGO_LLF:

            insert_here = (task->laxity < curr->laxity);
            break;
        }
        
        if (insert_here) {
            break;
        }
        
        prev = curr;
        curr = curr->next;
    }
    
    if (prev == NULL) {
        task->next = ready_queue;
        ready_queue = task;
    } else {
        task->next = prev->next;
        prev->next = task;
    }
}

static void remove_ready(rt_task_t *task)
{
    if (task == NULL || ready_queue == NULL) {
        return;
    }
    
    if (ready_queue == task) {
        ready_queue = task->next;
        task->next = NULL;
        return;
    }
    
    rt_task_t *prev = ready_queue;
    while (prev->next != NULL && prev->next != task) {
        prev = prev->next;
    }
    
    if (prev->next == task) {
        prev->next = task->next;
        task->next = NULL;
    }
}

void realtime_init(void)
{

    free_tasks = NULL;
    for (int i = RT_MAX_TASKS - 1; i >= 0; i--) {
        task_pool[i].next = free_tasks;
        free_tasks = &task_pool[i];
    }
    
    ready_queue = NULL;
    all_tasks = NULL;
    current_task = NULL;
    task_count = 0;
    system_time = 0;
    current_algo = RT_DEFAULT_ALGO;
    
    memset(&stats, 0, sizeof(stats));
    
    realtime_ops.init = realtime_init;
    realtime_ops.shutdown = realtime_shutdown;
    realtime_ops.schedule = realtime_schedule;
    realtime_ops.yield = realtime_yield;
    realtime_ops.preempt = realtime_preempt;
    realtime_ops.enqueue = realtime_enqueue;
    realtime_ops.dequeue = realtime_dequeue;
    realtime_ops.tick = realtime_tick;
    realtime_ops.get_stats = (void (*)(void *))realtime_get_stats;
    realtime_ops.print_stats = realtime_print_stats;
    realtime_ops.type = SCHED_EDF;
    realtime_ops.name = "realtime";
}

void realtime_shutdown(void)
{

    rt_task_t *task = all_tasks;
    while (task != NULL) {
        rt_task_t *next = task->next;
        free_task(task);
        task = next;
    }
    
    ready_queue = NULL;
    all_tasks = NULL;
    current_task = NULL;
    task_count = 0;
}

scheduler_ops_t *realtime_get_ops(void)
{
    return &realtime_ops;
}

void realtime_set_algorithm(rt_algorithm_t algo)
{
    if (algo == current_algo) {
        return;
    }
    
    current_algo = algo;
    
    switch (algo) {
    case RT_ALGO_RMS:
        rms_assign_priorities();
        break;
    case RT_ALGO_DMS:
        dms_assign_priorities();
        break;
    case RT_ALGO_LLF:
        llf_update_laxity();
        break;
    default:
        break;
    }
    
    rt_task_t *old_queue = ready_queue;
    ready_queue = NULL;
    
    while (old_queue != NULL) {
        rt_task_t *task = old_queue;
        old_queue = old_queue->next;
        task->next = NULL;
        insert_ready(task);
    }
}

rt_algorithm_t realtime_get_algorithm(void)
{
    return current_algo;
}

rt_task_t *edf_pick_next(void)
{

    return ready_queue;
}

void edf_enqueue(rt_task_t *task)
{
    insert_ready(task);
}

bool edf_check_schedulability(void)
{
    double util = realtime_calc_utilization();
    return util <= 1.0;
}

rt_task_t *rms_pick_next(void)
{

    return ready_queue;
}

void rms_assign_priorities(void)
{

    rt_task_t *sorted[RT_MAX_TASKS];
    int count = 0;
    
    rt_task_t *task = all_tasks;
    while (task != NULL && count < RT_MAX_TASKS) {
        sorted[count++] = task;
        task = task->next;
    }
    
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (sorted[j]->params.period > sorted[j+1]->params.period) {
                rt_task_t *tmp = sorted[j];
                sorted[j] = sorted[j+1];
                sorted[j+1] = tmp;
            }
        }
    }
    
    for (int i = 0; i < count; i++) {
        sorted[i]->rms_priority = count - i;
    }
}

double rms_utilization_bound(uint32_t n)
{
    if (n == 0) return 0.0;
    /* U <= n * (2^(1/n) - 1) */
    return n * (pow(2.0, 1.0/n) - 1.0);
}

bool rms_check_schedulability(void)
{
    if (task_count == 0) return true;
    
    double util = realtime_calc_utilization();
    double bound = rms_utilization_bound(task_count);
    
    stats.utilization = util;
    stats.schedulability_bound = bound;
    stats.schedulable = (util <= bound);
    
    return stats.schedulable;
}

rt_task_t *dms_pick_next(void)
{
    return ready_queue;
}

void dms_assign_priorities(void)
{

    rt_task_t *sorted[RT_MAX_TASKS];
    int count = 0;
    
    rt_task_t *task = all_tasks;
    while (task != NULL && count < RT_MAX_TASKS) {
        sorted[count++] = task;
        task = task->next;
    }
    
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (sorted[j]->params.deadline > sorted[j+1]->params.deadline) {
                rt_task_t *tmp = sorted[j];
                sorted[j] = sorted[j+1];
                sorted[j+1] = tmp;
            }
        }
    }
    
    for (int i = 0; i < count; i++) {
        sorted[i]->rms_priority = count - i;
    }
}

rt_task_t *llf_pick_next(void)
{

    llf_update_laxity();
    
    rt_task_t *min_task = NULL;
    int64_t min_laxity = INT64_MAX;
    
    rt_task_t *task = ready_queue;
    while (task != NULL) {
        if (task->state == RT_STATE_READY && task->laxity < min_laxity) {
            min_laxity = task->laxity;
            min_task = task;
        }
        task = task->next;
    }
    
    return min_task;
}

void llf_update_laxity(void)
{
    rt_task_t *task = all_tasks;
    while (task != NULL) {
        if (task->state == RT_STATE_READY || task->state == RT_STATE_RUNNING) {

            task->laxity = (int64_t)task->absolute_deadline - 
                          (int64_t)system_time - 
                          (int64_t)task->remaining_time;
        }
        task = task->next;
    }
}

void realtime_schedule(void)
{
    rt_task_t *next = NULL;
    
    switch (current_algo) {
    case RT_ALGO_EDF:
        next = edf_pick_next();
        break;
    case RT_ALGO_RMS:
        next = rms_pick_next();
        break;
    case RT_ALGO_DMS:
        next = dms_pick_next();
        break;
    case RT_ALGO_LLF:
        next = llf_pick_next();
        break;
    }
    
    if (next == NULL) {

        current_task = NULL;
        return;
    }
    
    remove_ready(next);
    
    if (next != current_task) {
        pid32 old_pid = (current_task != NULL) ? current_task->pid : -1;
        
        if (current_task != NULL && current_task->state == RT_STATE_RUNNING) {
            current_task->state = RT_STATE_READY;
            insert_ready(current_task);
            stats.preemptions++;
        }
        
        current_task = next;
        current_task->state = RT_STATE_RUNNING;
        current_task->start_time = system_time;
        
        stats.context_switches++;
        
        extern void context_switch(pid32 old, pid32 new);
        context_switch(old_pid, next->pid);
    }
}

void realtime_yield(void)
{
    if (current_task != NULL) {

        uint64_t elapsed = system_time - current_task->start_time;
        if (elapsed < current_task->remaining_time) {
            current_task->remaining_time -= elapsed;
        } else {
            current_task->remaining_time = 0;
        }
        
        current_task->state = RT_STATE_READY;
        insert_ready(current_task);
        current_task = NULL;
    }
    
    realtime_schedule();
}

void realtime_preempt(void)
{
    realtime_schedule();
}

bool realtime_check_preempt(void)
{
    if (current_task == NULL) {
        return ready_queue != NULL;
    }
    
    if (ready_queue == NULL) {
        return false;
    }
    
    switch (current_algo) {
    case RT_ALGO_EDF:
        return ready_queue->absolute_deadline < current_task->absolute_deadline;
        
    case RT_ALGO_RMS:
    case RT_ALGO_DMS:
        return ready_queue->rms_priority > current_task->rms_priority;
        
    case RT_ALGO_LLF:
        llf_update_laxity();
        return ready_queue->laxity < current_task->laxity;
    }
    
    return false;
}

void realtime_enqueue(pid32 pid)
{
    rt_task_t *task = find_task(pid);
    
    if (task == NULL) {

        rt_task_params_t params = {
            .period = RT_DEFAULT_PERIOD,
            .deadline = RT_DEFAULT_DEADLINE,
            .wcet = RT_DEFAULT_WCET,
            .phase = 0,
            .miss_policy = RT_DEFAULT_MISS_POLICY
        };
        realtime_create_task(pid, &params);
        task = find_task(pid);
    }
    
    if (task != NULL && task->state != RT_STATE_READY && 
        task->state != RT_STATE_RUNNING) {

        realtime_release(task);
    }
}

void realtime_dequeue(pid32 pid)
{
    rt_task_t *task = find_task(pid);
    if (task == NULL) {
        return;
    }
    
    remove_ready(task);
    
    if (all_tasks == task) {
        all_tasks = task->next;
    } else {
        rt_task_t *prev = all_tasks;
        while (prev != NULL && prev->next != task) {
            prev = prev->next;
        }
        if (prev != NULL) {
            prev->next = task->next;
        }
    }
    
    if (current_task == task) {
        current_task = NULL;
    }
    
    task_count--;
    free_task(task);
}

int realtime_create_task(pid32 pid, rt_task_params_t *params)
{
    if (params == NULL) {
        return -1;
    }
    
    if (find_task(pid) != NULL) {
        return -1;
    }
    
    rt_task_t *task = alloc_task();
    if (task == NULL) {
        return -1;
    }
    
    task->pid = pid;
    task->params = *params;
    task->state = RT_STATE_INACTIVE;
    task->remaining_time = params->wcet;
    
    task->next = all_tasks;
    all_tasks = task;
    task_count++;
    
    switch (current_algo) {
    case RT_ALGO_RMS:
        rms_assign_priorities();
        break;
    case RT_ALGO_DMS:
        dms_assign_priorities();
        break;
    default:
        task->rms_priority = 1;
        break;
    }
    
    return 0;
}

int realtime_set_params(pid32 pid, rt_task_params_t *params)
{
    rt_task_t *task = find_task(pid);
    if (task == NULL || params == NULL) {
        return -1;
    }
    
    task->params = *params;
    
    if (current_algo == RT_ALGO_RMS) {
        rms_assign_priorities();
    } else if (current_algo == RT_ALGO_DMS) {
        dms_assign_priorities();
    }
    
    return 0;
}

int realtime_get_params(pid32 pid, rt_task_params_t *params)
{
    rt_task_t *task = find_task(pid);
    if (task == NULL || params == NULL) {
        return -1;
    }
    
    return 0;
}

void realtime_release(rt_task_t *task)
{
    if (task == NULL) {
        return;
    }
    
    task->release_time = system_time;
    task->absolute_deadline = system_time + task->params.deadline;
    task->remaining_time = task->params.wcet;
    task->state = RT_STATE_READY;
    task->instances++;
    
    if (current_algo == RT_ALGO_LLF) {
        task->laxity = (int64_t)task->params.deadline - (int64_t)task->params.wcet;
    }
    
    insert_ready(task);
    stats.total_releases++;
    
    if (realtime_check_preempt()) {
        realtime_schedule();
    }
}

void realtime_complete(pid32 pid)
{
    rt_task_t *task = find_task(pid);
    if (task == NULL) {
        return;
    }
    
    uint64_t response_time = system_time - task->release_time;
    task->total_response_time += response_time;
    if (response_time > task->worst_response_time) {
        task->worst_response_time = response_time;
    }
    
    uint64_t exec_time = task->params.wcet - task->remaining_time;
    task->total_exec_time += exec_time;
    
    task->state = RT_STATE_COMPLETED;
    task->completions++;
    stats.total_completions++;
    
    update_stats_completion(task);
    
    if (current_task == task) {
        current_task = NULL;
    }
    
    remove_ready(task);
    
    realtime_schedule();
}

bool realtime_check_deadline(rt_task_t *task)
{
    if (task == NULL) {
        return false;
    }
    
    return system_time > task->absolute_deadline;
}

void realtime_handle_miss(rt_task_t *task)
{
    if (task == NULL) {
        return;
    }
    
    task->state = RT_STATE_MISSED;
    task->deadline_misses++;
    stats.total_deadline_misses++;
    
    switch (task->params.miss_policy) {
    case RT_MISS_SKIP:

        remove_ready(task);
        break;
        
    case RT_MISS_CONTINUE:

        break;
        
    case RT_MISS_ABORT:

        remove_ready(task);
        if (current_task == task) {
            current_task = NULL;
        }
        break;
        
    case RT_MISS_NOTIFY:

        kprintf("RT: Deadline miss for PID %d at time %llu\n",
                task->pid, system_time);
        break;
    }
}

void realtime_tick(void)
{
    system_time++;
    
    if (current_task != NULL && current_task->state == RT_STATE_RUNNING) {
        if (current_task->remaining_time > 0) {
            current_task->remaining_time--;
        }
        
        if (current_task->remaining_time == 0) {
            realtime_complete(current_task->pid);
        }
    }
    
    realtime_check_deadlines();
    
    realtime_check_releases();
    
    if (current_algo == RT_ALGO_LLF) {
        llf_update_laxity();
        
        rt_task_t *old_queue = ready_queue;
        ready_queue = NULL;
        while (old_queue != NULL) {
            rt_task_t *task = old_queue;
            old_queue = old_queue->next;
            task->next = NULL;
            insert_ready(task);
        }
    }
    
    if (realtime_check_preempt()) {
        realtime_schedule();
    }
}

void realtime_check_releases(void)
{
    rt_task_t *task = all_tasks;
    
    while (task != NULL) {
        if (task->state == RT_STATE_COMPLETED || 
            task->state == RT_STATE_MISSED ||
            task->state == RT_STATE_INACTIVE) {
            
            uint64_t next_release = task->release_time + task->params.period;
            
            if (system_time >= next_release) {
                realtime_release(task);
            }
        }
        task = task->next;
    }
}

void realtime_check_deadlines(void)
{
    rt_task_t *task = all_tasks;
    
    while (task != NULL) {
        if ((task->state == RT_STATE_READY || task->state == RT_STATE_RUNNING) &&
            realtime_check_deadline(task)) {
            realtime_handle_miss(task);
        }
        task = task->next;
    }
}

double realtime_calc_utilization(void)
{
    double util = 0.0;
    
    rt_task_t *task = all_tasks;
    while (task != NULL) {
        if (task->params.period > 0) {
            util += (double)task->params.wcet / task->params.period;
        }
        task = task->next;
    }
    
    stats.utilization = util;
    return util;
}

bool realtime_is_schedulable(void)
{
    switch (current_algo) {
    case RT_ALGO_EDF:
        return edf_check_schedulability();
        
    case RT_ALGO_RMS:
        return rms_check_schedulability();
        
    case RT_ALGO_DMS:
    case RT_ALGO_LLF:

        return edf_check_schedulability();
    }
    
    return false;
}

uint64_t realtime_response_time(rt_task_t *task)
{
    if (task == NULL) {
        return 0;
    }
    
    uint64_t r = task->params.wcet;
    uint64_t r_prev;
    
    do {
        r_prev = r;
        
        rt_task_t *hp_task = all_tasks;
        while (hp_task != NULL) {
            if (hp_task != task && hp_task->rms_priority > task->rms_priority) {
                uint64_t interference = ((r + hp_task->params.period - 1) / 
                                        hp_task->params.period) * hp_task->params.wcet;
                r = task->params.wcet + interference;
            }
            hp_task = hp_task->next;
        }
        
        if (r > task->params.deadline) {
            return r;
        }
        
    } while (r != r_prev);
    
    return r;
}

static void update_stats_completion(rt_task_t *task)
{

    realtime_calc_utilization();
}

void realtime_get_stats(rt_stats_t *s)
{
    if (s == NULL) {
        return;
    }
    
    s->utilization = realtime_calc_utilization();
    s->schedulable = realtime_is_schedulable();
    
    if (current_algo == RT_ALGO_RMS) {
        s->schedulability_bound = rms_utilization_bound(task_count);
    } else {
        s->schedulability_bound = 1.0;
    }
}

void realtime_reset_stats(void)
{
    stats.total_releases = 0;
    stats.total_completions = 0;
    stats.total_deadline_misses = 0;
    stats.preemptions = 0;
    stats.context_switches = 0;
    
    rt_task_t *task = all_tasks;
    while (task != NULL) {
        task->instances = 0;
        task->completions = 0;
        task->deadline_misses = 0;
        task->total_response_time = 0;
        task->worst_response_time = 0;
        task->total_exec_time = 0;
        task = task->next;
    }
}

void realtime_print_stats(void)
{
    const char *algo_names[] = {"EDF", "RMS", "DMS", "LLF"};
    
    kprintf("\n=== Real-Time Scheduler Statistics ===\n");
    kprintf("Algorithm: %s\n", algo_names[current_algo]);
    kprintf("System time: %llu ticks\n", system_time);
    kprintf("Total tasks: %u\n", task_count);
    kprintf("Utilization: %.2f%%\n", stats.utilization * 100);
    
    if (current_algo == RT_ALGO_RMS) {
        kprintf("RMS bound: %.2f%% (n=%u)\n", 
                rms_utilization_bound(task_count) * 100, task_count);
    }
    
    kprintf("Schedulable: %s\n", realtime_is_schedulable() ? "yes" : "no");
    kprintf("\nTotal releases: %llu\n", stats.total_releases);
    kprintf("Total completions: %llu\n", stats.total_completions);
    kprintf("Total deadline misses: %llu\n", stats.total_deadline_misses);
    kprintf("Preemptions: %llu\n", stats.preemptions);
    kprintf("Context switches: %llu\n", stats.context_switches);
    
    kprintf("\nPer-task statistics:\n");
    realtime_print_tasks();
}

void realtime_print_tasks(void)
{
    rt_task_t *task = all_tasks;
    
    while (task != NULL) {
        realtime_print_task(task);
        task = task->next;
    }
}

void realtime_print_task(rt_task_t *task)
{
    const char *state_names[] = {
        "INACTIVE", "READY", "RUNNING", "BLOCKED", "COMPLETED", "MISSED"
    };
    
    if (task == NULL) {
        return;
    }
    
    kprintf("  PID %d [%s]:\n", task->pid, state_names[task->state]);
    kprintf("    Period=%u, Deadline=%u, WCET=%u\n",
            task->params.period, task->params.deadline, task->params.wcet);
    kprintf("    Priority=%u, Remaining=%llu\n",
            task->rms_priority, task->remaining_time);
    kprintf("    Abs deadline=%llu, Release=%llu\n",
            task->absolute_deadline, task->release_time);
    kprintf("    Instances=%llu, Completions=%llu, Misses=%llu\n",
            task->instances, task->completions, task->deadline_misses);
    
    if (task->completions > 0) {
        kprintf("    Avg response=%.2f, Worst response=%llu\n",
                (double)task->total_response_time / task->completions,
                task->worst_response_time);
    }
}

bool realtime_validate(void)
{
    bool valid = true;
    
    uint32_t ready_count = 0;
    rt_task_t *task = ready_queue;
    rt_task_t *prev = NULL;
    
    while (task != NULL) {
        ready_count++;
        
        if (prev != NULL) {
            switch (current_algo) {
            case RT_ALGO_EDF:
                if (task->absolute_deadline < prev->absolute_deadline) {
                    kprintf("RT validate: EDF order violated\n");
                    valid = false;
                }
                break;
            case RT_ALGO_RMS:
            case RT_ALGO_DMS:
                if (task->rms_priority > prev->rms_priority) {
                    kprintf("RT validate: RMS/DMS order violated\n");
                    valid = false;
                }
                break;
            default:
                break;
            }
        }
        
        if (task->state != RT_STATE_READY) {
            kprintf("RT validate: task in ready queue not READY state\n");
            valid = false;
        }
        
        prev = task;
        task = task->next;
    }
    
    return valid;
}

rt_task_t *realtime_get_task(pid32 pid)
{
    return find_task(pid);
}

void realtime_set_time(uint64_t time)
{
    system_time = time;
}

uint64_t realtime_get_time(void)
{
    return system_time;
}
