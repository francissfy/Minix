/* This file contains the scheduling policy for SCHED
 *
 * The entry points are:
 *   do_noquantum:        Called on behalf of process' that run out of quantum
 *   do_start_scheduling  Request to start scheduling a proc
 *   do_stop_scheduling   Request to stop scheduling a proc
 *   do_nice		  Request to change the nice level on a proc
 *   init_scheduling      Called from main.c to set up/prepare scheduling
 */
#include "sched.h"
#include "schedproc.h"
#include <time.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
#include "kernel/proc.h" /* for queue constants */

#define SCHEDULE_DEFAULT 0
#define SCHEDULE_LOTTERY 1
#define SCHEDULE_EDF 2
static int schedule_type = SCHEDULE_DEFAULT;

static timer_t sched_timer;
static unsigned balance_timeout;

#define BALANCE_TIMEOUT	5 /* how often to balance queues in seconds */

static int schedule_process(struct schedproc * rmp, unsigned flags);
static void balance_queues(struct timer *tp);

static timer_t edf_timer;
static unsigned edf_timeout;
static clock_t edf_clock;

static void set_edf_timer(struct timer* tp);

#define SCHEDULE_CHANGE_PRIO	0x1
#define SCHEDULE_CHANGE_QUANTUM	0x2
#define SCHEDULE_CHANGE_CPU	0x4

#define SCHEDULE_CHANGE_ALL	(	\
	SCHEDULE_CHANGE_PRIO	|	\
	SCHEDULE_CHANGE_QUANTUM	|	\
	SCHEDULE_CHANGE_CPU		\
)

#define schedule_process_local(p)	\
	schedule_process(p, SCHEDULE_CHANGE_PRIO | SCHEDULE_CHANGE_QUANTUM)
#define schedule_process_migrate(p)	\
	schedule_process(p, SCHEDULE_CHANGE_CPU)

#define CPU_DEAD	-1

#define cpu_is_available(c)	(cpu_proc[c] >= 0)

#define DEFAULT_USER_TIME_SLICE 200

/* processes created by RS are sysytem processes */
#define is_system_proc(p)	((p)->parent == RS_PROC_NR)

static unsigned cpu_proc[CONFIG_MAX_CPUS];

static void pick_cpu(struct schedproc * proc)
{
#ifdef CONFIG_SMP
	unsigned cpu, c;
	unsigned cpu_load = (unsigned) -1;

	if (machine.processors_count == 1) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* schedule sysytem processes only on the boot cpu */
	if (is_system_proc(proc)) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* if no other cpu available, try BSP */
	cpu = machine.bsp_id;
	for (c = 0; c < machine.processors_count; c++) {
		/* skip dead cpus */
		if (!cpu_is_available(c))
			continue;
		if (c != machine.bsp_id && cpu_load > cpu_proc[c]) {
			cpu_load = cpu_proc[c];
			cpu = c;
		}
	}
	proc->cpu = cpu;
	cpu_proc[cpu]++;
#else
	proc->cpu = 0;
#endif
}


static int nice_to_priority(int nice, unsigned* new_q) {
	/* PRIO_MIN, PRIO_MAX defined in sys/resources */
	if (nice < PRIO_MIN || nice > PRIO_MAX) {
		return EINVAL;
	}
	*new_q = MAX_USER_Q + (nice-PRIO_MIN)*(MIN_USER_Q-MAX_USER_Q+1)/(PRIO_MAX-PRIO_MIN+1);

	/* some code that i don't know why */
	if ((signed)*new_q < MAX_USER_Q) {
		*new_q = MAX_USER_Q;
	}
	if (*new_q > MIN_USER_Q) {
		*new_q = MIN_USER_Q;
	}
	return OK;
}

/*===========================================================================*
 *				do_noquantum				     *
 *===========================================================================*/

int do_noquantum(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;

	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",
			   m_ptr->m_source);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	switch (schedule_type) {
	case SCHEDULE_DEFAULT:
		/* the default way of handling no quantum */
		/* push down the priority level */
		if (rmp->priority < MIN_USER_Q) {
			rmp->priority++;
		}
		if ((rv = schedule_process_local(rmp)) != OK) {
			return rv;
		}
		return OK;
	case SCHEDULE_LOTTERY:
		/* MAX_USER_Q is 0,  MIN_USER_Q is 15, defined in include/minix/config.h */
		/* directly dump to the lowest level */
		if (rmp->priority >= MAX_USER_Q && rmp->priority <= MIN_USER_Q) {
			rmp->priority = MIN_USER_Q;
		}
		if ((rv = schedule_process_local(rmp)) != OK) {
			return rv;
		}
		return lottery_scheduling();
	case SCHEDULE_EDF:
		if (rmp->priority >= MAX_USER_Q && rmp->priority <= MIN_USER_Q) {
			rmp->priority = MIN_USER_Q;
		}
		if ((rv = schedule_process_local(rmp)) != OK) {
			return rv;
		}
		return edf_scheduling();
	default:
		assert(0);
	}
}

/*===========================================================================*
 *				do_stop_scheduling			     *
 *===========================================================================*/
int do_stop_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int proc_nr_n;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
			   "%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
#ifdef CONFIG_SMP
	cpu_proc[rmp->cpu]--;
#endif
	rmp->flags = 0; /*&= ~IN_USE;*/

	switch (schedule_type) {
	case SCHEDULE_DEFAULT:
		return OK;
	case SCHEDULE_LOTTERY:
		return lottery_scheduling();
	case SCHEDULE_EDF:
		return edf_scheduling();
	default:
		assert(0);
	}
}

/*===========================================================================*
 *				do_start_scheduling			     *
 *===========================================================================*/
int do_start_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n, parent_nr_n;
	unsigned new_q;

	/* we can handle two kinds of messages here */
	assert(m_ptr->m_type == SCHEDULING_START ||
		   m_ptr->m_type == SCHEDULING_INHERIT);

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	/* Resolve endpoint to proc slot. */
	if ((rv = sched_isemtyendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n)) != OK) {
		return rv;
	}
	rmp = &schedproc[proc_nr_n];

	/* 
	 * convert nice to priority 
	 * m_ptr->SCHEDULING_MAXPRIO: scheduling max priority
	 */
	if ((rv = nice_to_priority(m_ptr->SCHEDULING_MAXPRIO, &new_q)) != OK) {
		new_q = MIN_USER_Q;
	}

	/* Populate process slot */
	rmp->endpoint     = m_ptr->SCHEDULING_ENDPOINT;
	rmp->parent       = m_ptr->SCHEDULING_PARENT;
	rmp->max_priority = new_q;
	/* init lottery number and deadline */
	rmp->lottery_num = 1;
	rmp->deadline = 0;
	if (rmp->max_priority >= NR_SCHED_QUEUES) {
		return EINVAL;
	}

	/* Inherit current priority and time slice from parent. Since there
	 * is currently only one scheduler scheduling the whole system, this
	 * value is local and we assert that the parent endpoint is valid */
	if (rmp->endpoint == rmp->parent) {
		/* We have a special case here for init, which is the first
		   process scheduled, and the parent of itself. */
		rmp->priority   = USER_Q;
		rmp->time_slice = DEFAULT_USER_TIME_SLICE;

		/*
		 * Since kernel never changes the cpu of a process, all are
		 * started on the BSP and the userspace scheduling hasn't
		 * changed that yet either, we can be sure that BSP is the
		 * processor where the processes run now.
		 */
#ifdef CONFIG_SMP
		rmp->cpu = machine.bsp_id;
		/* FIXME set the cpu mask */
#endif
	}

	switch (m_ptr->m_type) {

	case SCHEDULING_START:
		// printf("INFO: do_start_scheduling: SCHEDULING_START\n");
		/* We have a special case here for system processes, for which
		 * quanum and priority are set explicitly rather than inherited 
		 * from the parent */
		switch (schedule_type) {
		case SCHEDULE_DEFAULT:
			// printf("INFO: do_start_scheduling: SCHEDULE_DEFAULT\n");
			rmp->priority = rmp->max_priority;
			break;

		/* in both cases, priority is the lowest
		 * process scheduled in scheduler
		 */
		case SCHEDULE_LOTTERY:
		case SCHEDULE_EDF:
			// printf("INFO: do_start_scheduling: SCHEDULE_EDF or SCHEDULE_LOTTERY\n");
			rmp->priority = MIN_USER_Q;
			break;
		default:
			assert(0);
		}
		rmp->time_slice = (unsigned)m_ptr->SCHEDULING_QUANTUM;
		break;

	case SCHEDULING_INHERIT:
		/* Inherit current priority and time slice from parent. Since there
		 * is currently only one scheduler scheduling the whole system, this
		 * value is local and we assert that the parent endpoint is valid */
		if ((rv = sched_isokendpt(m_ptr->SCHEDULING_PARENT,
								  &parent_nr_n)) != OK)
			return rv;

		rmp->priority = schedproc[parent_nr_n].priority;
		rmp->time_slice = schedproc[parent_nr_n].time_slice;
		break;

	default:
		/* not reachable */
		assert(0);
	}

	/* Take over scheduling the process. The kernel reply message populates
	 * the processes current priority and its time slice */
	if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0, 0)) != OK) {
		printf("Sched: Error taking over scheduling for %d, kernel said %d\n",
			   rmp->endpoint, rv);
		return rv;
	}
	rmp->flags = IN_USE;

	/* Schedule the process, giving it some quantum */
	pick_cpu(rmp);
	while ((rv = schedule_process(rmp, SCHEDULE_CHANGE_ALL)) == EBADCPU) {
		/* don't try this CPU ever again */
		cpu_proc[rmp->cpu] = CPU_DEAD;
		pick_cpu(rmp);
	}

	if (rv != OK) {
		printf("Sched: Error while scheduling process, kernel replied %d\n",
			   rv);
		return rv;
	}

	/* Mark ourselves as the new scheduler.
	 * By default, processes are scheduled by the parents scheduler. In case
	 * this scheduler would want to delegate scheduling to another
	 * scheduler, it could do so and then write the endpoint of that
	 * scheduler into SCHEDULING_SCHEDULER
	 */

	m_ptr->SCHEDULING_SCHEDULER = SCHED_PROC_NR;

	return OK;
}

/*===========================================================================*
 *				do_nice					     *
 *===========================================================================*/
int do_nice(message *m_ptr)
{
	struct schedproc *rmp;
	int rv;
	int proc_nr_n;
	unsigned new_q, old_q, old_max_q;
	int nice;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
			   "%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	nice = m_ptr->SCHEDULING_MAXPRIO;

	switch (schedule_type) {
	case SCHEDULE_DEFAULT:
		/* original way of convering nice to priority */
		if ((rv = nice_to_priority(nice, &new_q)) != OK) {
			return rv;
		}
		if (new_q >= NR_SCHED_QUEUES) {
			return EINVAL;
		}

		old_q = rmp->priority;
		old_max_q = rmp->max_priority;

		rmp->max_priority = rmp->priority = new_q;

		if ((rv = schedule_process_local(rmp)) != OK) {
			rmp->priority = old_q;
			rmp->max_priority = old_max_q;
		}

		return rv;

	case SCHEDULE_LOTTERY:
		/* set nice as number of lottery tickets */
		if (nice < 1) {
			nice = 1;
		}
		rmp->lottery_num = nice;
		printf("INFO: do_nice: set lottery num: %d, endpoint: %d\n", nice, rmp->endpoint);
		return OK;
	case SCHEDULE_EDF:
		/* change deadline by nice, deadline is the time to complete */
		if (nice <= 0) {
			rmp->deadline = 0;
		} else {
			rmp->deadline = edf_clock + (double)sys_hz()/1000.0f*(double)nice;
		}
		printf("INFO: do_nice: set deadline: %d, endpoint: %d\n", (int)rmp->deadline, rmp->endpoint);
		return OK;
	default:
		assert(0);
	}
}

/*===========================================================================*
 *				schedule_process			     *
 *===========================================================================*/
static int schedule_process(struct schedproc* rmp, unsigned flags) {
	int err;
	int new_prio, new_quantum, new_cpu;

	pick_cpu(rmp);

	if (flags & SCHEDULE_CHANGE_PRIO)
		new_prio = rmp->priority;
	else
		new_prio = -1;

	if (flags & SCHEDULE_CHANGE_QUANTUM)
		new_quantum = rmp->time_slice;
	else
		new_quantum = -1;

	if (flags & SCHEDULE_CHANGE_CPU)
		new_cpu = rmp->cpu;
	else
		new_cpu = -1;

	if ((err = sys_schedule(rmp->endpoint, new_prio, 
		new_quantum, new_cpu)) != OK) {
		printf("PM: An error occurred when trying to schedule %d: %d\n",
			   rmp->endpoint, err);
	}

	return err;
}

/*===========================================================================*
 *				start_scheduling			     *
 *===========================================================================*/

void init_scheduling(void)
{
	balance_timeout = BALANCE_TIMEOUT * sys_hz();
	init_timer(&sched_timer);
	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
	edf_clock = 0;
	edf_timeout = sys_hz()/10;
	init_timer(&edf_timer);
	set_timer(&edf_timer, edf_timeout, set_edf_timer, 0);
}

static void set_edf_timer(struct timer* tp) {
	edf_clock += edf_timeout;
	/* timer, tick, watch dog */
	set_timer(&edf_timer, edf_timeout, set_edf_timer, 0);
}

/*===========================================================================*
 *				balance_queues				     *
 *===========================================================================*/

/* This function in called every 100 ticks to rebalance the queues. The current
 * scheduler bumps processes down one priority when ever they run out of
 * quantum. This function will find all proccesses that have been bumped down,
 * and pulls them back up. This default policy will soon be changed.
 */
static void balance_queues(struct timer *tp)
{
	struct schedproc *rmp;
	int proc_nr;

	switch (schedule_type) {
	case SCHEDULE_DEFAULT:
		for (proc_nr=0, rmp=schedproc; proc_nr<NR_PROCS; proc_nr++, rmp++) {
			if ((rmp->flags & IN_USE)
					&& (rmp->priority > rmp->max_priority)) {
				rmp->priority--;
				schedule_process_local(rmp);
			}
		}
		break;
	/* for these two scheduler, do nothing */
	case SCHEDULE_LOTTERY:
	case SCHEDULE_EDF:
		break;
	default:
		assert(0);
	}

	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}

/*
 * works like re-balancing the queues are achieved in this method
 */
int lottery_scheduling(void) {
	struct schedproc *rmp;
	unsigned total;
	unsigned ticket;
	unsigned now;
	unsigned num_schedproc;
	int i;

	total = 0;
	num_schedproc = 0;
	/* 
	 * NR_PROCS: Number of slots in the process table for non-kernel processes
	 * IN_USE: shedproc in in use
	 * rmp->priority == MIN_USER_Q: initially set?
	 */

	for (i=0, rmp=schedproc; i<NR_PROCS; ++i, ++rmp) {
		if ((rmp->flags & IN_USE)
				&& rmp->priority==MIN_USER_Q) {
			total += rmp->lottery_num;
			num_schedproc ++;
		}
	}
	printf("INFO: lottery_scheduling: number sched procs: %d total tickets:%d\n", num_schedproc, total);

	/* this case is no process to schedule? */
	if (!total) {
		// printf("INFO: lottery_scheduling: lottery total: %d\n", total);
		return OK;
	}

	/*
	 * schedproc[256]: list of process info?
	 * USER_Q: ((MIN_USER_Q - MAX_USER_Q) / 2 + MAX_USER_Q)
	 */
	ticket = random() % total + 1;
	now = 0;
	for (i=0, rmp=schedproc; i<NR_PROCS; ++i, ++rmp) {
		if ((rmp->flags & IN_USE) && rmp->priority == MIN_USER_Q) {
			if ((now += rmp->lottery_num) >= ticket) {
				rmp->priority = USER_Q;
				printf("INFO: lottery_scheduling: lottery lucy ticket: %d, total: %d, endpoint (pid): %d\n", ticket, total, rmp->endpoint);
				schedule_process_local(rmp);
				return OK;
			}
		}
	}
	return OK;
}

int edf_scheduling(void) {
	struct schedproc *rmp, *min_rmp;
	clock_t min_deadline;
	int i;

	min_deadline = 0;
	min_rmp = NULL;
	for (i=0, rmp=schedproc; i<NR_PROCS; ++i,++rmp) {
		/* pick the eaerlest deadline */
		if ((rmp->flags & IN_USE) 
				&& rmp->priority == MIN_USER_Q
				&& (!min_rmp || (rmp->deadline && rmp->deadline < min_deadline))) {
			min_deadline = rmp->deadline;
			min_rmp = rmp;
		}
	}

	if (!min_rmp) {
		// printf("INFO: edf_scheduling: earliest deadline not found\n");
		return OK;
	}

	printf("INFO: edf_scheduling: earliest deadline: %d\n", min_deadline);
	min_rmp->priority = USER_Q;
	schedule_process_local(min_rmp);
	return OK;
}

void switch_schedule_type(void) {
	schedule_type = (schedule_type+1)%3;
	printf("INFO: switch_schedule_type: switch to %d\n(schedulers SCHEDULE_DEFAULT 0; SCHEDULE_LOTTERY 1; SCHEDULE_EDF 2;)\n", schedule_type);
}