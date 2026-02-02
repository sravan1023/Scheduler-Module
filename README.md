# Process Schedulers

A comprehensive collection of CPU scheduling algorithms implemented in C, featuring multiple scheduling policies with a pluggable architecture.

## Overview

This implements six major scheduling algorithms commonly used in operating systems, each with distinct characteristics and use cases. The implementation features a unified interface with detailed statistics tracking and support for dynamic policy switching.

## Scheduling Algorithms

### 1. Round-Robin (RR)

Time-sharing scheduler that allocates equal CPU time to all processes in a circular queue.

**Key Features:**
- Circular doubly-linked queue structure
- Configurable time quantum (default: 10 ticks)
- O(1) enqueue/dequeue complexity
- Fair CPU distribution

**Best For:** General-purpose systems with similar-priority processes

---

### 2. Priority Scheduling

Preemptive priority-based scheduler with aging mechanism to prevent starvation.

**Key Features:**
- 100 priority levels (0-99, higher = more urgent)
- Priority aging for waiting processes
- I/O completion bonus for interactive tasks
- Starvation prevention mechanism

**Best For:** Systems requiring differentiated process importance

---

### 3. Multi-Level Feedback Queue (MLFQ)

Adaptive scheduler that dynamically adjusts process priorities based on behavior.

**Key Features:**
- 8 priority levels with exponentially increasing time quantums
- Automatic priority demotion for CPU-bound processes
- Priority boost on I/O completion
- Periodic anti-starvation boost
- Per-queue statistics tracking

**Queue Configuration:**

| Level | Time Quantum | Time Allotment |
|-------|--------------|----------------|
| 0     | 2 ticks      | 8 ticks        |
| 1     | 4 ticks      | 16 ticks       |
| 2     | 8 ticks      | 32 ticks       |
| 3     | 16 ticks     | 64 ticks       |
| 4     | 32 ticks     | 128 ticks      |
| 5     | 64 ticks     | 256 ticks      |
| 6     | 128 ticks    | 512 ticks      |
| 7     | 256 ticks    | 1024 ticks     |

**Best For:** Mixed workloads with both interactive and CPU-intensive processes

---

### 4. Lottery Scheduling

Probabilistic fair-share scheduler using lottery tickets for proportional CPU allocation.

**Key Features:**
- Ticket-based proportional sharing
- Dynamic ticket transfers between processes
- Compensation tickets for voluntary yields
- Fairness metrics (Jain's fairness index)
- Randomized selection for fairness

**Best For:** Proportional-share resource allocation scenarios

---

### 5. Completely Fair Scheduler (CFS)

Linux-inspired fair scheduler using virtual runtime and red-black tree for O(log n) operations.

**Key Features:**
- Virtual runtime (vruntime) fairness metric
- Nice values: -20 (highest) to +19 (lowest)
- Red-black tree for efficient task selection
- Sleeper bonus for I/O-bound tasks
- Minimum granularity to prevent thrashing

**Best For:** Desktop and server systems requiring fairness with priority control

---

### 6. Real-Time Scheduling

Hard and soft real-time scheduler supporting multiple deadline-driven algorithms.

**Supported Algorithms:**
- **EDF** (Earliest Deadline First)
- **RMS** (Rate-Monotonic Scheduling)
- **DMS** (Deadline-Monotonic Scheduling)
- **LLF** (Least Laxity First)

**Key Features:**
- Periodic task support with phases
- Deadline miss detection and handling
- WCET (Worst-Case Execution Time) tracking
- Configurable miss policies (skip, continue, abort, notify)
- Admission control for schedulability

**Best For:** Real-time systems with timing constraints

---

## Architecture

### Core Components

- **scheduler.h/c**: Main scheduler framework with unified interface
- **Pluggable design**: Easy switching between scheduling policies
- **Statistics engine**: Comprehensive tracking of scheduler metrics

### Statistics Tracked

- Context switches (voluntary/involuntary)
- Wait time, turnaround time, response time
- Queue lengths and preemption counts
- Algorithm-specific metrics (vruntime, tickets, deadlines)
