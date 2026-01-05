#include "sched.h"
#include "output.h"
#include "timer.h"

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
    struct task_struct *curr = current; 
    
    while (1) {
        struct task_struct *next_task = 0;
        int curr_idx = -1;
        
        // Find current index
        for (int i = 0; i < NR_TASKS_PER_CORE; i++) {
            if (&task[core_id][i] == curr) {
                curr_idx = i;
                break;
            }
        }
        
        if (curr_idx != -1) {
            int next_idx = (curr_idx + 1) % NR_TASKS_PER_CORE;
            // Find next RUNNING task
            for (int i = 0; i < NR_TASKS_PER_CORE; i++) {
                int idx = (next_idx + i) % NR_TASKS_PER_CORE;
                if (task[core_id][idx].state == TASK_RUNNING) {
                    next_task = &task[core_id][idx];
                    break;
                }
            }
        }
        
        if (next_task) {
            if (next_task != curr) {
                struct task_struct *prev = curr;
                current_tasks[core_id] = next_task;
                cpu_switch_to(prev, next_task);
            }
            return;
        }
        
        // No other task found.
        if (curr->state == TASK_RUNNING) {
            return; // Continue current
        }
        
        // Current is sleeping/zombie, and no other task.
        // We MUST wait.
        asm volatile("msr daifclr, #2"); // Ensure IRQs enabled
        asm volatile("wfi");
        // After IRQ (Timer), check again.
    }
}

extern void *pmm_alloc_page();

// Create a thread on the CURRENT core
int kthread_create(void (*fn)(void)) {
    unsafe_puts("DEBUG: kthread_create called\n");
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

void sleep_ms(uint64_t ms) {
    uint64_t freq = get_timer_freq();
    uint64_t ticks = (freq * ms) / 1000;
    uint64_t current_time;
    asm volatile("mrs %0, cntpct_el0" : "=r"(current_time));
    
    current->wake_time = current_time + ticks;
    current->state = TASK_SLEEPING;
    schedule();
}

void timer_tick() {
    int core_id = get_core_id();
    uint64_t current_time;
    asm volatile("mrs %0, cntpct_el0" : "=r"(current_time));
    
    // Wake up sleeping tasks
    for (int i = 0; i < NR_TASKS_PER_CORE; i++) {
        struct task_struct *p = &task[core_id][i];
        if (p->state == TASK_SLEEPING && current_time >= p->wake_time) {
            p->state = TASK_RUNNING;
        }
    }

    --current->counter;
    // unsafe_puts("Tick "); unsafe_puthex(current->counter); unsafe_puts("\n");
    
    // If current is not running, we are likely in the idle loop.
    // Don't preempt. The idle loop will check for new tasks after we return.
    if (current->state != TASK_RUNNING) {
        return;
    }

    if (current->counter > 0 || current->preempt_count > 0) {
        return;
    }
    current->counter = current->priority; // Reset counter
    
    // unsafe_puts("Preempt\n");
    schedule();
}
