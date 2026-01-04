#ifndef _SCHED_H
#define _SCHED_H

#define THREAD_CPU_CONTEXT			0 		// offset of cpu_context in task_struct 

#ifndef __ASSEMBLER__
#include <stdint.h>

#define THREAD_SIZE				4096

#define NR_TASKS				20
#define NR_CORES                4
#define NR_TASKS_PER_CORE       5

#define FIRST_TASK task[get_core_id()][0]
#define LAST_TASK task[get_core_id()][NR_TASKS_PER_CORE-1]

#define TASK_RUNNING				0
#define TASK_ZOMBIE				1
#define TASK_SLEEPING               2

struct cpu_context {
	unsigned long x19;
	unsigned long x20;
	unsigned long x21;
	unsigned long x22;
	unsigned long x23;
	unsigned long x24;
	unsigned long x25;
	unsigned long x26;
	unsigned long x27;
	unsigned long x28;
	unsigned long fp;
	unsigned long sp;
	unsigned long pc;
};

struct task_struct {
	struct cpu_context cpu_context;
	long state;	
	long counter;
	long priority;
	long preempt_count;
    unsigned long spsr;
    unsigned long elr;
    long pid;
    long cpu;
    uint64_t wake_time;
};

extern struct task_struct *current_tasks[NR_CORES];
extern struct task_struct task[NR_CORES][NR_TASKS_PER_CORE];

int get_core_id(void);
#define current (current_tasks[get_core_id()])

void init_scheduler(void);
void schedule(void);
void cpu_switch_to(struct task_struct* prev, struct task_struct* next);
int kthread_create(void (*fn)(void));
void timer_tick(void);

#endif
#endif
