/* The object file of "table.c" contains most kernel data. Variables that 
 * are declared in the *.h files appear with EXTERN in front of them, as in
 *
 *    EXTERN int x;
 *
 * Normally EXTERN is defined as extern, so when they are included in another
 * file, no storage is allocated.  If EXTERN were not present, but just say,
 *
 *    int x;
 *
 * then including this file in several source files would cause 'x' to be
 * declared several times.  While some linkers accept this, others do not,
 * so they are declared extern when included normally.  However, it must be
 * declared for real somewhere.  That is done here, by redefining EXTERN as
 * the null string, so that inclusion of all *.h files in table.c actually
 * generates storage for them.  
 *
 * Various variables could not be declared EXTERN, but are declared PUBLIC
 * or PRIVATE. The reason for this is that extern variables cannot have a  
 * default initialization. If such variables are shared, they must also be
 * declared in one of the *.h files without the initialization.  Examples 
 * include 'tasktab' (this file) and 'idt'/'gdt' (protect.c). 
 *
 * Changes:
 *    Nov 10, 2004   removed controller->driver mappings  (Jorrit N. Herder)
 *    Oct 17, 2004   updated above and tasktab comments  (Jorrit N. Herder)
 *    Aug 18, 2004   included p_type in tasktab  (Jorrit N. Herder) 
 *    May 01, 2004   included p_sendmask in tasktab  (Jorrit N. Herder)
 */

#define _TABLE

#include "kernel.h"
#include "proc.h"
#include "sendmask.h"
#include <minix/com.h>
#include <ibm/int86.h>

/* Define stack sizes for all tasks included in the system image. */
#define NO_STACK	0
#define SMALL_STACK	(128 * sizeof(char *))
#if (CHIP == INTEL)
#define	IDLE_STACK	((3+3+4) * sizeof(char *))  /* 3 intr, 3 temps, 4 db */
#else
#define IDLE_STACK	SMALL_STACK
#endif
#define	HARDWARE_STACK	NO_STACK	/* dummy task, uses kernel stack */
#define	SYS_STACK	SMALL_STACK
#define	CLOCK_STACK	SMALL_STACK

/* Stack space for all the task stacks.  Declared as (char *) to align it. */
#define	TOT_STACK_SPACE	(IDLE_STACK+HARDWARE_STACK+CLOCK_STACK+SYS_STACK)
PUBLIC char *t_stack[TOT_STACK_SPACE / sizeof(char *)];
	

/* The system image table lists all programs that are part of the boot image. 
 * The order of the entries here MUST agree with the order of the programs
 * in the boot image and all kernel tasks must come first.
 * Each entry provides the process number, type, scheduling priority, send
 * mask, and a name for the process table. For kernel processes, the startup 
 * routine and stack size is also provided.
 */
PUBLIC struct system_image image[] = {
 { IDLE, idle_task,    P_IDLE,   PPRI_IDLE,  IDLE_STACK,    IDLE_SENDMASK,    "IDLE"    },
 { CLOCK, clock_task,   P_TASK,   PPRI_TASK, CLOCK_STACK,   CLOCK_SENDMASK,   "CLOCK"   },
 { SYSTASK, sys_task,     P_TASK,   PPRI_TASK, SYS_STACK,     SYSTEM_SENDMASK,  "SYS"     },
 { HARDWARE, 0,            P_TASK,   PPRI_TASK, HARDWARE_STACK,HARDWARE_SENDMASK,"HARDWAR" },
 { PM_PROC_NR, 0,            P_SERVER, PPRI_NORMAL, 0,             PM_SENDMASK,      "PM"      },
 { FS_PROC_NR, 0,            P_SERVER, PPRI_NORMAL, 0,             FS_SENDMASK,      "FS"      },
 { IS_PROC_NR, 0,            P_SYSTEM, PPRI_HIGHER, 0,             IS_SENDMASK,      "IS"      },
 { TTY, 0,            P_SYSTEM, PPRI_HIGHER, 0,             TTY_SENDMASK,      "TTY"      },
 { MEMORY, 0,            P_DRIVER, PPRI_HIGH, 0,             MEM_SENDMASK,     "MEMORY" },
#if ENABLE_AT_WINI
 { AT_WINI, 0,            P_DRIVER, PPRI_HIGHER, 0,           AT_SENDMASK,      "AT_WINI" },
#endif
#if ENABLE_FLOPPY
 { FLOPPY, 0,            P_DRIVER, PPRI_HIGH, 0,             FLOPPY_SENDMASK,  "FLOPPY" },
#endif
#if ENABLE_PRINTER
 { PRINTER, 0,            P_DRIVER, PPRI_NORMAL, 0,           PRN_SENDMASK,     "PRINTER" },
#endif
#if ENABLE_RTL8139
 { USR8139, 0,            P_DRIVER, PPRI_HIGH, 0,             RTL8139_SENDMASK,  "RTL8139" },
#endif
 { INIT_PROC_NR, 0,            P_USER,   PPRI_USER, 0,             INIT_SENDMASK,    "INIT"    },
};

/* Verify the size of the system image table at compile time. If the number 
 * is not correct, the size of the 'dummy' array will be negative, causing
 * a compile time error. Note that no space is allocated because 'dummy' is
 * declared extern.
  */
extern int dummy[(IMAGE_SIZE==sizeof(image)/sizeof(struct system_image))?1:-1];

