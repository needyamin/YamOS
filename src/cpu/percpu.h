/* YamKernel — Per-CPU data accessed via GS_BASE */
#ifndef _CPU_PERCPU_H
#define _CPU_PERCPU_H

#include <nexus/types.h>

struct task;

typedef struct percpu {
    u32 cpu_id;             /* +0  */
    u32 apic_id;            /* +4  */
    struct task *current;   /* +8  */
    struct task *idle;      /* +16 */
    u64 ticks;              /* +24 */
    u64 user_rsp_save;      /* +32  syscall scratch (asm uses gs:[32]) */
    u64 kernel_rsp;         /* +40  syscall kernel stack (asm uses gs:[40]) */
    u64 idle_ticks;         /* +48  ticks spent in idle loop */
    /* Absolute addresses of syscall_entry stack slots (valid during syscall_dispatch). */
    u64 syscall_rip_slot;   /* +56  points to saved user RIP (syscall clobber slot) */
    u64 syscall_rsp_slot;   /* +64  points to saved user RSP slot */
    u64 syscall_rdi_slot;   /* +72  points to saved user RDI slot */
    u64 syscall_resume_rsp; /* +80  kernel RSP at syscall_entry `call syscall_dispatch` site */
} percpu_t;

void      percpu_init(u32 cpu_id, u32 apic_id);
void      percpu_set_kernel_rsp(u64 rsp);
percpu_t *this_cpu(void);

#endif
