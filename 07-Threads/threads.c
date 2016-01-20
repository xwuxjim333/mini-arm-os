#include <stdint.h>
#include "threads.h"
#include "os.h"
#include "malloc.h"
#include "reg.h"

#define THREAD_PSP	0xFFFFFFFD

#define MAX_ORDER 9

/* Thread Control Block */
typedef struct {
	void *stack;
	void *orig_stack;
	uint8_t in_use;
	uint8_t priority;
} tcb_t;

static tcb_t tasks[MAX_TASKS];
static int lastTask;
static int first = 1;
static int task_order[MAX_ORDER] = {3, 1, 2, 1, 1, 1, 1, 2, 2};
static int robin = 0;
static int now;

/* FIXME: Without naked attribute, GCC will corrupt r7 which is used for stack
 * pointer. If so, after restoring the tasks' context, we will get wrong stack
 * pointer.
 */
void __attribute__((naked)) pendsv_handler()
{
	/* Save the old task's context */
	asm volatile("mrs   r0, psp\n"
	             "stmdb r0!, {r4-r11, lr}\n");
	/* To get the task pointer address from result r0 */
	asm volatile("mov   %0, r0\n" : "=r" (tasks[lastTask].stack));
	now = tasks[lastTask].priority;
	/* Find a new task to run */
	
	while (1) {

		for (lastTask = 0; lastTask < MAX_TASKS; lastTask++) {
				if (task_order[robin] == now) {
					robin++;
					if (robin == MAX_ORDER)
							robin = 0;
				}
				if (tasks[lastTask].priority == task_order[robin] && tasks[lastTask].in_use) {
					/* Move the task's stack pointer address into r0 */
					/* Restore the new task's context and jump to the task */
					asm volatile("mov r0, %0\n"
								 "ldmia r0!, {r4-r11, lr}\n"
							     "msr psp, r0\n"
							     "bx lr\n"
								 : : "r" (tasks[lastTask].stack));
					now = task_order[robin];
					robin++;
					lastTask++;
					if (robin == MAX_ORDER)
						robin = 0;
				}	
		}
		
	}
}

void systick_handler()
{
	*SCB_ICSR |= SCB_ICSR_PENDSVSET;
}

void thread_start()
{
	lastTask = 0;
	robin = 0;
	/* Save kernel context */
	asm volatile("mrs ip, psr\n"
	             "push {r4-r11, ip, lr}\n");

	/* To bridge the variable in C and the register in ASM,
	 * move the task's stack pointer address into r0.
	 * http://www.ethernut.de/en/documents/arm-inline-asm.html
	 */
	asm volatile("mov r0, %0\n" : : "r" (tasks[lastTask].stack));
	/* Load user task's context and jump to the task */
	asm volatile("msr psp, r0\n"
	             "mov r0, #3\n"
	             "msr control, r0\n"
	             "isb\n"
	             "pop {r4-r11, lr}\n"
	             "pop {r0}\n"
	             "bx lr\n");
}

int thread_create(void (*run)(void *), void *userdata, uint8_t p)
{
	/* Find a free thing */
	int threadId = 0;
	uint32_t *stack;

	for (threadId = 0; threadId < MAX_TASKS; threadId++) {
		if (tasks[threadId].in_use == 0)
			break;
	}

	if (threadId == MAX_TASKS)
		return -1;

	/* Create the stack */
	stack = (uint32_t *) malloc(STACK_SIZE * sizeof(uint32_t));
	tasks[threadId].orig_stack = stack;
	if (stack == 0)
		return -1;

	stack += STACK_SIZE - 32; /* End of stack, minus what we are about to push */
	if (first) {
		stack[8] = (unsigned int) run;
		stack[9] = (unsigned int) userdata;
		first = 0;
	} else {
		stack[8] = (unsigned int) THREAD_PSP;
		stack[9] = (unsigned int) userdata;
		stack[14] = (unsigned) &thread_self_terminal;
		stack[15] = (unsigned int) run;
		stack[16] = (unsigned int) 0x21000000; /* PSR Thumb bit */
	}

	/* Construct the control block */
	tasks[threadId].stack = stack;
	tasks[threadId].in_use = 1;
	tasks[threadId].priority = p;

	return threadId;
}

void thread_kill(int thread_id)
{
	tasks[thread_id].in_use = 0;

	/* Free the stack */
	free(tasks[thread_id].orig_stack);
}

void thread_self_terminal()
{
	/* This will kill the stack.
	 * For now, disable context switches to save ourselves.
	 */
	asm volatile("cpsid i\n");
	thread_kill(lastTask);
	asm volatile("cpsie i\n");

	/* And now wait for death to kick in */
	while (1);
}

