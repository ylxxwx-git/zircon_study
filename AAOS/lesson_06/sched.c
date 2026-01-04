#include "sched.h"
#include "output.h"

struct task_struct task[NR_CORES][NR_TASKS_PER_CORE];
struct task_struct *current_tasks[NR_CORES];

void init_scheduler(void) {
    int core_id = get_core_id();
    
    // Initialize Task 0 for THIS core
    struct task_struct *p = &task[core_id][0];
    current_tasks[core_id] = p;
    
    p->state = TASK_RUNNING;
    p->priority = 1;
    p->counter = p->priority;
    p->preempt_count = 0;
    p->pid = core_id * 100 + 0; // PID format: CoreID * 100 + TaskID
    p->cpu = core_id;
    
    // Initialize other tasks as empty/zombie for now
    for (int i = 1; i < NR_TASKS_PER_CORE; i++) {
        task[core_id][i].state = TASK_ZOMBIE;
        task[core_id][i].pid = core_id * 100 + i;
        task[core_id][i].cpu = core_id;
    }
    
    if (core_id == 0) {
        puts("Scheduler Initialized. Per-Core PCB Arrays.\n");
    }
}

void schedule(void) {
    int core_id = get_core_id();
    // unsafe_puts("Schedule Core "); unsafe_puthex(core_id); unsafe_puts("\n");
    
    struct task_struct *curr = current; 
    
    // Simple Round Robin within local array
    struct task_struct *next_task = 0;
    
    // Calculate current task index
    // We can assume curr is within task[core_id] array
    // But let's be safe and just search
    
    int curr_idx = -1;
    for (int i = 0; i < NR_TASKS_PER_CORE; i++) {
        if (&task[core_id][i] == curr) {
            curr_idx = i;
            break;
        }
    }
    
    if (curr_idx == -1) return; // Should not happen
    
    int next_idx = (curr_idx + 1) % NR_TASKS_PER_CORE;
    
    // Find next RUNNING task
    for (int i = 0; i < NR_TASKS_PER_CORE; i++) {
        int idx = (next_idx + i) % NR_TASKS_PER_CORE;
        // unsafe_puts("Checking Task "); unsafe_puthex(idx); unsafe_puts(" State "); unsafe_puthex(task[core_id][idx].state); unsafe_puts("\n");
        if (task[core_id][idx].state == TASK_RUNNING) {
            next_task = &task[core_id][idx];
            break;
        }
    }
    
    if (next_task && next_task != curr) {
        // unsafe_puts("Switching from "); unsafe_puthex(curr->pid);
        // unsafe_puts(" to "); unsafe_puthex(next_task->pid); unsafe_puts("\n");
        struct task_struct *prev = curr;
        current_tasks[core_id] = next_task;
        cpu_switch_to(prev, next_task);
    }
}

extern void *pmm_alloc_page();

// Create a thread on the CURRENT core
int kthread_create(void (*fn)(void)) {
    int core_id = get_core_id();
    struct task_struct *p = 0;
    
    // Find free PCB in local array
    for (int i = 0; i < NR_TASKS_PER_CORE; i++) {
        if (task[core_id][i].state == TASK_ZOMBIE) {
            p = &task[core_id][i];
            break;
        }
    }
    
    if (!p) return -1;
    
    // Allocate Stack
    void *stack = pmm_alloc_page();
    if (!stack) return -2;
    
    p->state = TASK_RUNNING;
    p->counter = 1;
    p->priority = 1;
    p->preempt_count = 0;
    
    p->cpu_context.x19 = (unsigned long)fn;
    p->cpu_context.x20 = 0;
    extern void ret_from_fork(void);
    p->cpu_context.pc = (unsigned long)ret_from_fork;
    p->cpu_context.sp = (unsigned long)stack + 4096;
    
    return p->pid;
}

void timer_tick() {
    --current->counter;
    // unsafe_puts("Tick "); unsafe_puthex(current->counter); unsafe_puts("\n");
    if (current->counter > 0 || current->preempt_count > 0) {
        return;
    }
    current->counter = current->priority; // Reset counter
    
    // unsafe_puts("Preempt\n");
    schedule();
}
