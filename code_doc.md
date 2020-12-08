```c
/* include/minix/com.h
 * 
 * define schedule message
 * for talking between kernel and user space 
 */
#define SCHEDULING_SWITCH_TYPE (SCHEDULING_BASE+6)

/* servers/is/dmp.c
 * 
 * struct hook_entry, map function key with switching scheduler
 */
struct hook_entry {
	int key;
	void (*function)(void);
	char *name;
} hooks[] = {
	{ F1, 	proctab_dmp, "Kernel process table" },
	{ F3,	image_dmp, "System image" },
	{ F4,	privileges_dmp, "Process privileges" },
	{ F5,	monparams_dmp, "Boot monitor parameters" },
	{ F6,	irqtab_dmp, "IRQ hooks and policies" },
	{ F7,	kmessages_dmp, "Kernel messages" },
	{ F8,	vm_dmp, "VM status and process maps" },
	{ F10,	kenv_dmp, "Kernel parameters" },
	{ SF1,	mproc_dmp, "Process manager process table" },
	{ SF2,	sigaction_dmp, "Signals" },
	{ SF3,	fproc_dmp, "Filesystem process table" },
	{ SF4,	dtab_dmp, "Device/Driver mapping" },
	{ SF5,	mapping_dmp, "Print key mappings" },
	{ SF6,	rproc_dmp, "Reincarnation server process table" },

    /* newly added */
    { SF7,  schedule_switch, "Swtch the schedule type"},

	{ SF8,  data_store_dmp, "Data store contents" },
	{ SF9,  procstack_dmp, "Processes with stack traces" },
};


/* servers/is/dmp_pm.c
 * 
 * SCHED_PROC_NR: User-space processes, device drivers, servers, and INIT.
 * SCHEDULING_SWITCH_TYPE: self-defined switch scheduler message
 * _taskcall: kernel call
 * call switch scheduler from kernel to scheduler
 */
void schedule_switch(void) {
	message m;
	_taskcall(SCHED_PROC_NR, SCHEDULING_SWITCH_TYPE, &m);
}


/* servers/is/proto.h
 * 
 * implemented in servers/is/dmp_pm.c
 */
void schedule_switch(void);

/* servers/pm/proto.h
 * 
 * implemented in service/pm/schedule.c
 */
void do_switch_schedule(void);


/* service/pm/schedule.c
 * 
 * directly nice as the priority level
 * in rmp->mp_nice < 0? 0: rmp->mp_nice, maxprio
 */
int sched_start_user(endpoint_t ep, struct mproc *rmp) {
	unsigned maxprio;
	endpoint_t inherit_from;
	int rv;
	
	/* scheduler must know the parent, which is not the case for a child
	 * of a system process created by a regular fork; in this case the 
	 * scheduler should inherit settings from init rather than the real 
	 * parent
	 */
	if (mproc[rmp->mp_parent].mp_flags & PRIV_PROC) {
		assert(mproc[rmp->mp_parent].mp_scheduler == NONE);
		inherit_from = INIT_PROC_NR;
	} else {
		inherit_from = mproc[rmp->mp_parent].mp_endpoint;
	}
	
	/* inherit quantum */
	return sched_inherit(ep, 			/* scheduler_e */
		rmp->mp_endpoint, 			/* schedulee_e */
		inherit_from, 				/* parent_e */
		rmp->mp_nice < 0? 0: rmp->mp_nice, /* maxprio */
		&rmp->mp_scheduler);			/* *newsched_e */
}


/* service/pm/schedule.c
 * 
 * use nice as the scheduling max priority
 */
int sched_nice(struct mproc *rmp, int nice)
{
	int rv;
	message m;
	unsigned maxprio;

	/* If the kernel is the scheduler, we don't allow messing with the
	 * priority. If you want to control process priority, assign the process
	 * to a user-space scheduler */
	if (rmp->mp_scheduler == KERNEL || rmp->mp_scheduler == NONE)
		return (EINVAL);

	m.SCHEDULING_ENDPOINT	= rmp->mp_endpoint;
	m.SCHEDULING_MAXPRIO	= nice;
	if ((rv = _taskcall(rmp->mp_scheduler, SCHEDULING_SET_NICE, &m))) {
		return rv;
	}

	return (OK);
}

/* service/pm/schedule.c
 * 
 * mproc: struct defined in servers/pm/mproc.h
 * mproc->mp_scheduler: User space scheduling, scheduler endpoint id
 * m: empty message here
 */
void do_switch_schedule(void) {
	message m;
	_taskcall(mproc->mp_scheduler, SCHEDULING_SWITCH_TYPE, &m);
}


/* servers/sched/main.c
 * 
 * handling the massage from kernel? (maybe)
 */
/*
 * new case SCHEDULING_SWITCH_TYPE:
 * switch_schedule_type()
 */


/* servers/sched/proto.h
 * 
 * function declaration
 */
int lottery_scheduling(void);
int edf_scheduling(void);
void switch_schedule_type(void);


/* servers/sched/schedproc.h
 * 
 * add lottery_num, deadline to shedproc struct
 * new timer header
 */
EXTERN struct schedproc {
	endpoint_t endpoint;	/* process endpoint id */
	endpoint_t parent;	/* parent endpoint id */
	unsigned flags;		/* flag bits */

	/* User space scheduling */
	unsigned max_priority;	/* this process' highest allowed priority */
	unsigned priority;		/* the process' current priority */
	unsigned time_slice;		/* this process's time slice */
	unsigned cpu;		/* what CPU is the process running on */
	bitchunk_t cpu_mask[BITMAP_CHUNKS(CONFIG_MAX_CPUS)]; /* what CPUs is hte
								process allowed
								to run on */
	/* lottery scheduling */
	unsigned lottery_num;
	/* edf scheduling */
	clock_t deadline;
} schedproc[NR_PROCS];


/* servers/sched/schedule.c
 * 
 * switch between DEFAULT, LOTTERY, EDF
 */
void switch_schedule_type(void) {
    switch (schedule_type) {
    case SCHEDULE_DEFAULT:
        schedule_type = SCHEDULE_LOTTERY;
        printf("Schedule type changed to LOTTERY mode\n");
        break;
    case SCHEDULE_LOTTERY:
        schedule_type = SCHEDULE_EDF;
        printf("Schedule type changed to EDF mode\n");
        break;
    case SCHEDULE_EDF:
        schedule_type = SCHEDULE_DEFAULT;
        printf("Schedule type changed to DEFAULT mode\n");
        break;
    default:
        assert(0);
    }
}


/* servers/sched/schedule.c
 * 
 * convert nice level to priority
 */
static int nice_to_priority(int nice, unsigned* new_q) {
	/* PRIO_MIN, PRIO_MAX defined in sys/resources */
	if (nice < PRIO_MIN || nice > PRIO_MAX) {
		return EINVAL;
	}

	/* linear project from PRIO_MIN...PRIO_MAX to MIN_USER_Q...MAX_USER_Q*/
	*new_q = MAX_USER_Q + (nice-PRIO_MIN)*(MIN_USER_Q-MAX_USER_Q+1)/(PRIO_MAX-PRIO_MIN+1);

	/* why choosing only  MIN_USER_Q and MAX_USER_Q*/
	if ((signed)*new_q < MAX_USER_Q) {
		*new_q = MAX_USER_Q;
	}
	if (*new_q > MIN_USER_Q) {
		*new_q = MIN_USER_Q;
	}
	return OK;
}

/* servers/sched/schedule.c
 * 
 * handle no quantum
 * directly dump to lowest level
 * set rmp->priority => schedule local => corresponding scheduler 
 */
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
		if (rmp->priority < MIN_USER_Q) {
			rmp->priority ++;
		}
		if ((rv = schedule_process_local(rmp)) != OK) {
			return rv;
		}
		return OK;
	case SCHEDULE_LOTTERY:
		/* adjust the priority level */
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


/* servers/sched/schedule.c
 * 
 * handle stop scheduling
 * set flag and call scheduler
 */
int do_stop_scheduling(message *m_ptr) {
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

/* 害 感觉还没直接写注释来的方便 */





```

