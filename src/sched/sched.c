/* YamKernel — Multi-Queue CFS Scheduler v0.3.0
 * Sorted-array O(log n) pick, per-CPU queues, fork, nice, load balance */
#include "sched.h"
#include "wait.h"
#include "../cpu/percpu.h"
#include "../cpu/apic.h"
#include "../cpu/idt.h"
#include "../cpu/gdt.h"
#include "../mem/heap.h"
#include "../mem/slab.h"
#include "../mem/vmm.h"
#include "../mem/pmm.h"
#include "../fs/vfs.h"
#include "../lib/spinlock.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../cpu/cpuid.h"
#include "../cpu/smp.h"
#include "../cpu/msr.h"
#include "../nexus/graph.h"

static kmem_cache_t *task_cache;
extern void task_trampoline(void);
extern void fork_child_trampoline(void);

/* Linux-style nice-to-weight mapping (subset) */
static const u32 nice_to_weight[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */ 9548,  7620,  6100,  4904,  3906,
    /*  -5 */ 3121,  2501,  1991,  1586,  1277,
    /*   0 */ 1024,  820,   655,   526,   423,
    /*   5 */ 335,   272,   215,   172,   137,
    /*  10 */ 110,   87,    70,    56,    45,
    /*  15 */ 36,    29,    23,    18,    15,
};

#define NICE_TO_IDX(n) ((n) + 20)
#define SCHED_INTERACTIVE_QUANTUM_TICKS 2
#define SCHED_LATENCY_TICKS 8
#define SCHED_MIN_GRANULARITY_TICKS 1
#define SCHED_MAX_GRANULARITY_TICKS 6

/* Per-CPU run queues */
static runqueue_t per_cpu_rq[MAX_CPUS];
static task_t    *active_tasks[MAX_CPUS];
static task_t    *sleep_head;
static spinlock_t sleep_lock = SPINLOCK_INIT;
static u64        next_id = 1;
static volatile u32 sched_enabled = 0;
static u64        total_tasks = 0;
static u64        lifetime_tasks_created = 0;
static u64        lifetime_processes_forked = 0;
static u64        lifetime_threads_created = 0;
static u64        lifetime_tasks_reaped = 0;
static u64        lifetime_task_objects_freed = 0;
static u64        lifetime_kernel_stacks_freed = 0;
static u64        lifetime_user_pml4s_destroyed = 0;
static u64        lifetime_vma_lists_destroyed = 0;
static u64        lifetime_fd_tables_closed = 0;
static u64        lifetime_graph_nodes_destroyed = 0;
static u64        lifetime_orphans_detached = 0;
static u64        lifetime_detached_tasks_reaped = 0;
static task_t    *deferred_reap_head = NULL;
static spinlock_t deferred_reap_lock = SPINLOCK_INIT;

#define TASK_GRAPH_NODE_INVALID ((u32)~0u)

static bool sched_task_is_active(task_t *t) {
    if (!t) return false;
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (active_tasks[cpu] == t) return true;
    }
    return false;
}

static bool sched_task_in_runqueue(task_t *t) {
    if (!t) return false;
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) {
        runqueue_t *rq = &per_cpu_rq[cpu];
        u64 f = spin_lock_irqsave(&rq->lock);
        for (u32 i = 0; i < rq->count; i++) {
            if (rq->tasks[i] == t) {
                spin_unlock_irqrestore(&rq->lock, f);
                return true;
            }
        }
        spin_unlock_irqrestore(&rq->lock, f);
    }
    return false;
}

static void sched_drop_task_count(task_t *t) {
    if (!t || !t->accounted) return;
    if (total_tasks > 0) total_tasks--;
    t->accounted = 0;
}

static void sched_queue_deferred_reap(task_t *t) {
    if (!t || t->reap_queued) return;
    t->reap_queued = 1;
    u64 f = spin_lock_irqsave(&deferred_reap_lock);
    t->wait_next = deferred_reap_head;
    deferred_reap_head = t;
    spin_unlock_irqrestore(&deferred_reap_lock, f);
}

static void sched_unlink_from_parent(task_t *t) {
    if (!t || !t->parent) return;
    task_t *parent = t->parent;
    for (u32 i = 0; i < parent->child_count; i++) {
        if (parent->children[i] != t) continue;
        parent->children[i] = parent->children[parent->child_count - 1];
        parent->children[parent->child_count - 1] = NULL;
        parent->child_count--;
        break;
    }
    t->parent = NULL;
}

static void sched_destroy_task_resources(task_t *t,
                                         bool destroy_address_space,
                                         bool close_fds,
                                         bool free_object) {
    if (!t) return;

    if (destroy_address_space) {
        if (t->vma_head) lifetime_vma_lists_destroyed++;
        vmm_destroy_task_vmas(t);
        if (t->pml4) {
            vmm_destroy_user_pml4(t->pml4);
            t->pml4 = NULL;
            lifetime_user_pml4s_destroyed++;
        }
    }

    if (close_fds) {
        bool had_fds = false;
        for (u32 i = 0; i < 128; i++) {
            if (t->fd_table[i]) {
                had_fds = true;
                break;
            }
        }
        vfs_task_close_fds(t);
        if (had_fds) lifetime_fd_tables_closed++;
    }

    if (t->stack) {
        vmm_free_kernel_stack(t->stack, SCHED_STACK_SIZE);
        t->stack = NULL;
        lifetime_kernel_stacks_freed++;
    }
    if (t->fpu_state) {
        kfree(t->fpu_state);
        t->fpu_state = NULL;
    }
    if (t->graph_node != TASK_GRAPH_NODE_INVALID && t->graph_node != 0) {
        if (yamgraph_node_destroy(t->graph_node)) lifetime_graph_nodes_destroyed++;
        t->graph_node = TASK_GRAPH_NODE_INVALID;
    }

    if (free_object && task_cache) {
        lifetime_task_objects_freed++;
        kmem_cache_free(task_cache, t);
    }
}

static bool sched_task_owns_address_space(task_t *t) {
    return t && t->pml4 && t->thread_group == 0;
}

static void sched_reap_detached_dead_task(task_t *t, const char *reason) {
    if (!t || t == this_cpu()->current || sched_task_is_active(t) ||
        sched_task_in_runqueue(t)) {
        sched_queue_deferred_reap(t);
        return;
    }

    sched_unlink_from_parent(t);
    sched_drop_task_count(t);
    t->state = TASK_DEAD;
    t->reap_queued = 0;
    t->wait_next = NULL;
    lifetime_tasks_reaped++;
    lifetime_detached_tasks_reaped++;
    kprintf("[SCHED] cleanup dead task pid=%lu reason=%s\n",
            t->id, reason ? reason : "deferred");
    sched_destroy_task_resources(t, sched_task_owns_address_space(t), true, true);
}

static void sched_reap_deferred_dead_tasks(void) {
    task_t *list = NULL;
    u64 f = spin_lock_irqsave(&deferred_reap_lock);
    list = deferred_reap_head;
    deferred_reap_head = NULL;
    spin_unlock_irqrestore(&deferred_reap_lock, f);

    while (list) {
        task_t *next = list->wait_next;
        list->wait_next = NULL;
        list->reap_queued = 0;
        if (sched_task_is_active(list)) {
            sched_queue_deferred_reap(list);
        } else {
            sched_reap_detached_dead_task(list, "deferred");
        }
        list = next;
    }
}

static void sched_orphan_children(task_t *parent) {
    if (!parent || parent->child_count == 0) return;

    bool shared_address_space = false;
    for (u32 i = 0; i < parent->child_count; i++) {
        task_t *child = parent->children[i];
        if (!child) continue;
        if (parent->pml4 && child->pml4 == parent->pml4)
            shared_address_space = true;
        child->parent = NULL;
        lifetime_orphans_detached++;
        if (child->state == TASK_ZOMBIE || child->state == TASK_DEAD) {
            child->state = TASK_DEAD;
            sched_drop_task_count(child);
            if (!sched_task_in_runqueue(child))
                sched_queue_deferred_reap(child);
        }
        parent->children[i] = NULL;
    }
    parent->child_count = 0;

    if (shared_address_space) {
        kprintf("[SCHED] pid=%lu exited with live shared-address children; retaining pml4 for thread-group cleanup\n",
                parent->id);
        parent->pml4 = NULL;
        parent->vma_head = NULL;
    }
}

/* ---- Sorted run queue (binary insert for O(log n) pick) ---- */

static void rq_insert(runqueue_t *rq, task_t *t) {
    if (rq->count >= 256) return;
    t->state = TASK_READY;

    /* Binary search for insertion point (sorted by vruntime ascending) */
    u32 lo = 0, hi = rq->count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (rq->tasks[mid]->vruntime < t->vruntime) lo = mid + 1;
        else hi = mid;
    }

    /* Shift elements right */
    for (u32 i = rq->count; i > lo; i--) rq->tasks[i] = rq->tasks[i-1];
    rq->tasks[lo] = t;
    rq->count++;
    rq->load_weight += t->weight ? t->weight : 1;

    if (rq->count == 1 || t->vruntime < rq->min_vruntime)
        rq->min_vruntime = t->vruntime;
}

static task_t *rq_pick_next(runqueue_t *rq) {
    if (rq->count == 0) return NULL;
    /* First element has lowest vruntime */
    task_t *t = rq->tasks[0];
    /* Shift left */
    for (u32 i = 0; i < rq->count - 1; i++) rq->tasks[i] = rq->tasks[i+1];
    rq->count--;
    if (rq->load_weight >= (t->weight ? t->weight : 1)) rq->load_weight -= (t->weight ? t->weight : 1);
    else rq->load_weight = 0;
    rq->min_vruntime = (rq->count > 0) ? rq->tasks[0]->vruntime : 0;
    rq->nr_switches++;
    rq->last_pick_id = t->id;
    return t;
}

/* ---- Public API ---- */

task_t *sched_current(void) { return this_cpu()->current; }
u64 sched_task_count(void) { return total_tasks; }

static u32 weight_for_nice(i8 nice) {
    int idx = NICE_TO_IDX(nice);
    if (idx < 0) idx = 0;
    if (idx >= 40) idx = 39;
    return nice_to_weight[idx];
}

static u32 sched_effective_slice_ticks(runqueue_t *rq, task_t *t) {
    u32 runnable = rq->count + 1;
    if (runnable == 0) runnable = 1;

    u32 base = SCHED_LATENCY_TICKS / runnable;
    if (base < SCHED_MIN_GRANULARITY_TICKS) base = SCHED_MIN_GRANULARITY_TICKS;
    if (base > SCHED_MAX_GRANULARITY_TICKS) base = SCHED_MAX_GRANULARITY_TICKS;

    if (t && t->nice < 0 && base < SCHED_MAX_GRANULARITY_TICKS) base++;
    if (t && t->nice > 5 && base > SCHED_MIN_GRANULARITY_TICKS) base--;
    if (t && t->ai_hint == AI_HINT_REALTIME && base < SCHED_MAX_GRANULARITY_TICKS) base++;
    if (t && t->ai_hint == AI_HINT_BATCH && base > SCHED_MIN_GRANULARITY_TICKS) base--;
    return base;
}

static u32 sched_pick_cpu_for_task(task_t *t) {
    u32 sched_cpus = smp_sched_cpu_count();
    if (sched_cpus == 0) sched_cpus = 1;
    if (sched_cpus > MAX_CPUS) sched_cpus = MAX_CPUS;

    u32 best_cpu = 0;
    u32 best_count = 0xFFFFFFFFu;
    for (u32 cpu = 0; cpu < sched_cpus; cpu++) {
        if (t && !(t->cpu_affinity & (1ULL << cpu))) continue;
        runqueue_t *rq = &per_cpu_rq[cpu];
        u32 count = __atomic_load_n(&rq->count, __ATOMIC_RELAXED);
        if (count < best_count) {
            best_count = count;
            best_cpu = cpu;
        }
    }
    return best_cpu;
}

task_t *sched_spawn(const char *name, void (*entry)(void *), void *arg, u8 prio) {
    if (!task_cache)
        task_cache = kmem_cache_create("task_t", sizeof(task_t), 16);
    task_t *t = (task_t *)kmem_cache_alloc(task_cache);
    if (!t) return NULL;

    memset(t, 0, sizeof(*t));
    t->graph_node = TASK_GRAPH_NODE_INVALID;
    t->id = next_id++;
    t->prio = (prio < SCHED_PRIO_LEVELS) ? prio : SCHED_PRIO_LEVELS - 1;
    t->nice = DEFAULT_NICE;
    t->weight = weight_for_nice(t->nice);
    t->cpu_affinity = ~0ULL;  /* All CPUs allowed */
    t->entry = entry; t->arg = arg;
    t->parent = sched_current();
    t->child_count = 0;
    t->exit_code = 0;
    t->signal_pending = 0;
    t->ai_hint = AI_HINT_NONE;
    t->utime = t->stime = 0;
    t->vol_switches = t->invol_switches = 0;
    t->slice_ticks = 0;
    t->need_resched = 0;
    t->start_tick = this_cpu()->ticks;
    t->rss_pages = 0;
    t->brk_start = 0x400000ULL;
    t->brk_current = t->brk_start;
    t->cgroup = NULL;
    memset(t->name, 0, sizeof(t->name));
    for (u32 i = 0; i < sizeof(t->name) - 1 && name[i]; i++) t->name[i] = name[i];
    if (t->parent && t->parent->cwd[0]) {
        strncpy(t->cwd, t->parent->cwd, sizeof(t->cwd) - 1);
    } else {
        strncpy(t->cwd, "/", sizeof(t->cwd) - 1);
    }
    t->graph_node = yamgraph_node_create(YAM_NODE_TASK, t->name, t);
    if (t->graph_node == TASK_GRAPH_NODE_INVALID) {
        sched_destroy_task_resources(t, false, false, true);
        return NULL;
    }

    t->stack = (u8 *)vmm_alloc_kernel_stack(SCHED_STACK_SIZE);
    if (!t->stack) {
        sched_destroy_task_resources(t, false, false, true);
        return NULL;
    }

    u32 fpu_sz = cpuid_get_info()->xsave_size;
    if (fpu_sz == 0) fpu_sz = 512; /* FXSAVE fallback */
    t->fpu_state = (u8 *)kmalloc(fpu_sz);
    if (!t->fpu_state) {
        sched_destroy_task_resources(t, false, false, true);
        return NULL;
    }
    memset(t->fpu_state, 0, fpu_sz);

    /* Stack frame for context_switch + task_trampoline */
    u64 *sp = (u64 *)(t->stack + SCHED_STACK_SIZE);
    *--sp = (u64)entry;
    *--sp = (u64)arg;
    *--sp = (u64)task_trampoline;
    *--sp = 0;  /* rbp */
    *--sp = 0;  /* rbx */
    *--sp = 0;  /* r12 */
    *--sp = 0;  /* r13 */
    *--sp = 0;  /* r14 */
    *--sp = 0;  /* r15 */
    *--sp = 0x002;  /* RFLAGS */
    t->rsp = (u64)sp;

    /* Add to parent's children */
    if (t->parent && t->parent->child_count < MAX_CHILDREN) {
        t->parent->children[t->parent->child_count++] = t;
    }

    u32 target_cpu = sched_pick_cpu_for_task(t);
    t->cpu = target_cpu;
    runqueue_t *rq = &per_cpu_rq[target_cpu];
    u64 f = spin_lock_irqsave(&rq->lock);
    u64 minv = rq->min_vruntime;
    t->vruntime = minv;
    rq_insert(rq, t);
    spin_unlock_irqrestore(&rq->lock, f);

    total_tasks++;
    t->accounted = 1;
    lifetime_tasks_created++;
    kprintf("[SCHED] spawn '%s' id=%lu cpu=%u nice=%d w=%u rq_ready=%u rq_load=%lu\n",
            t->name, t->id, target_cpu, t->nice, t->weight, rq->count, rq->load_weight);
    return t;
}

void task_exit(void) {
    sched_exit_current(0);
    for (;;) {
        sched_yield();
        __asm__ volatile("hlt");
    }
}

void sched_exit_current(i32 code) {
    task_t *cur = sched_current();
    if (!cur) return;
    cur->exit_code = code;
    sched_orphan_children(cur);
    cur->state = cur->parent ? TASK_ZOMBIE : TASK_DEAD;
    if (!cur->parent) {
        sched_drop_task_count(cur);
        sched_queue_deferred_reap(cur);
    }
}

void sched_kill_task(u64 id) {
    if (id == 0 || id == 1 || id == 2) return; /* don't kill init tasks */
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        /* 1. Check if it's currently running on this CPU */
        task_t *active = active_tasks[cpu];
        if (active && active->id == id) {
            active->signal_pending |= (1 << SIGKILL);
            active->need_resched = 1;
        }
        
        /* 2. Check if it's waiting in the runqueue */
        runqueue_t *rq = &per_cpu_rq[cpu];
        u64 f = spin_lock_irqsave(&rq->lock);
        for (u32 i = 0; i < rq->count; i++) {
            if (rq->tasks[i]->id == id) {
                rq->tasks[i]->signal_pending |= (1 << SIGKILL);
            }
        }
        spin_unlock_irqrestore(&rq->lock, f);
    }
}

void sched_init(void) {
    /* Initialize per-CPU run queues */
    for (int i = 0; i < MAX_CPUS; i++) {
        per_cpu_rq[i].count = 0;
        per_cpu_rq[i].min_vruntime = 0;
        per_cpu_rq[i].load_weight = 0;
        per_cpu_rq[i].nr_switches = 0;
        per_cpu_rq[i].last_pick_id = 0;
        spin_init(&per_cpu_rq[i].lock);
    }

    task_t *boot = (task_t *)kmalloc(sizeof(task_t));
    if (!boot) { kprintf("[SCHED] FATAL: kmalloc failed for boot task!\n"); return; }
    memset(boot, 0, sizeof(*boot));
    boot->id     = 0;
    boot->prio   = SCHED_PRIO_LEVELS - 1;
    boot->nice   = DEFAULT_NICE;
    boot->weight = weight_for_nice(DEFAULT_NICE);
    boot->state  = TASK_RUNNING;
    boot->cpu_affinity = ~0ULL;
    boot->name[0] = 'b'; boot->name[1] = 's'; boot->name[2] = 'p';
    strncpy(boot->cwd, "/", sizeof(boot->cwd) - 1);
    boot->brk_start = 0x400000ULL;
    boot->brk_current = boot->brk_start;

    u32 fpu_sz_boot = cpuid_get_info()->xsave_size;
    if (fpu_sz_boot == 0) fpu_sz_boot = 512;
    boot->fpu_state = (u8 *)kmalloc(fpu_sz_boot);
    if (boot->fpu_state)
        memset(boot->fpu_state, 0, fpu_sz_boot);

    this_cpu()->current = boot;
    this_cpu()->idle    = boot;
    active_tasks[0]     = boot;
    total_tasks = 1;
    boot->accounted = 1;
    lifetime_tasks_created = 1;

    kprintf_color(0xFF00FF88, "[SCHED] Multi-Queue CFS v0.3.0 init OK (nice range %d..%d)\n",
                  MIN_NICE, MAX_NICE);
}

/* FPU Save/Restore — guards xgetbv behind has_xsave check */
static inline void fpu_save(u8 *state) {
    if (!state) return;
    if (cpuid_get_info()->has_xsave) {
        u32 lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        __asm__ volatile("xsave64 %0" : "=m"(*state) : "a"(lo), "d"(hi) : "memory");
    } else if (cpuid_get_info()->has_fxsr) {
        __asm__ volatile("fxsave64 %0" : "=m"(*state) : : "memory");
    }
}

static inline void fpu_restore(u8 *state) {
    if (!state) return;
    if (cpuid_get_info()->has_xsave) {
        u32 lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        __asm__ volatile("xrstor64 %0" : : "m"(*state), "a"(lo), "d"(hi) : "memory");
    } else if (cpuid_get_info()->has_fxsr) {
        __asm__ volatile("fxrstor64 %0" : : "m"(*state) : "memory");
    }
}

static void switch_to(task_t *next) {
    percpu_t *pc = this_cpu();
    task_t   *cur = pc->current;
    if (next->stack) {
        u64 ktop = (u64)(next->stack + SCHED_STACK_SIZE);
        gdt_set_kernel_stack(pc->cpu_id, ktop);
        pc->kernel_rsp = ktop;
    }

    fpu_save(cur->fpu_state);
    next->state = TASK_RUNNING;
    next->slice_ticks = 0;
    next->need_resched = 0;
    next->cpu = pc->cpu_id;
    pc->current = next;
    active_tasks[pc->cpu_id] = next;
    fpu_restore(next->fpu_state);
    
    if (next->tls_base) {
        wrmsr(MSR_FS_BASE, next->tls_base);
    }

    u64 *next_pml4 = next->pml4 ? next->pml4 : vmm_get_kernel_pml4();
    u64 *cur_pml4 = cur->pml4 ? cur->pml4 : vmm_get_kernel_pml4();
    if (next_pml4 != cur_pml4) {
        u64 phys = vmm_virt_hhdm_to_phys(next_pml4);
        __asm__ volatile("mov %0, %%cr3" :: "r"(phys) : "memory");
    }

    context_switch(&cur->rsp, next->rsp);
}

void sched_yield(void) {
    if (!sched_enabled) return;
    cli();
    sched_reap_deferred_dead_tasks();
    u32 cpu = 0; /* BSP for now */
    runqueue_t *rq = &per_cpu_rq[cpu];
    spin_lock(&rq->lock);
    task_t *cur = this_cpu()->current;
    task_t *next = rq_pick_next(rq);
    task_t *dead_picked[16];
    u32 dead_count = 0;
    
    /* Skip tasks that were killed while waiting in the queue */
    while (next && next->state == TASK_DEAD) {
        if (next->reap_queued) {
            next = rq_pick_next(rq);
            continue;
        }
        if (dead_count < 16) dead_picked[dead_count++] = next;
        else sched_queue_deferred_reap(next);
        next = rq_pick_next(rq);
    }
    
    if (!next) {
        spin_unlock(&rq->lock);
        for (u32 i = 0; i < dead_count; i++)
            sched_reap_detached_dead_task(dead_picked[i], "runqueue-dead");
        sti();
        return;
    }
    if (cur && cur->state == TASK_RUNNING) {
        cur->vol_switches++;
        cur->slice_ticks = 0;
        cur->need_resched = 0;
        rq_insert(rq, cur);
    }
    spin_unlock(&rq->lock);
    for (u32 i = 0; i < dead_count; i++)
        sched_reap_detached_dead_task(dead_picked[i], "runqueue-dead");
    switch_to(next);
    sti();
}

void sched_drain_deferred_reaps(void) {
    if (!sched_enabled) return;
    cli();
    sched_reap_deferred_dead_tasks();
    sti();
}

void sched_maybe_preempt(void) {
    task_t *cur = sched_current();
    if (!sched_enabled || !cur || !cur->need_resched) return;
    if (cur->state != TASK_RUNNING || cur == this_cpu()->idle) return;
    cur->need_resched = 0;
    sched_yield();
}

void sched_unblock(task_t *t) {
    u32 target_cpu = sched_pick_cpu_for_task(t);
    t->cpu = target_cpu;
    runqueue_t *rq = &per_cpu_rq[target_cpu];
    u64 f = spin_lock_irqsave(&rq->lock);
    if (t->state == TASK_BLOCKED || t->state == TASK_STOPPED) rq_insert(rq, t);
    spin_unlock_irqrestore(&rq->lock, f);
}

void sched_tick(void) {
    percpu_t *pc = this_cpu();
    pc->ticks++;
    task_t *cur = pc->current;
    if (cur) {
        cur->ticks++;
        cur->slice_ticks++;
        cur->stime++;
        /* Weight-based vruntime: lower weight = faster vruntime growth */
        cur->vruntime += (1024ULL * 1024) / (cur->weight ? cur->weight : 1);
        /* AI hint adjustments */
        if (cur->ai_hint == AI_HINT_BATCH)
            cur->vruntime -= 128; /* Slight advantage for batch jobs */
        if (cur == pc->idle) {
            pc->idle_ticks++;
        } else if (cur->slice_ticks >= sched_effective_slice_ticks(&per_cpu_rq[pc->cpu_id], cur)) {
            cur->need_resched = 1;
        }
    }

    /* Wake sleepers */
    u64 now = pc->ticks;
    spin_lock(&sleep_lock);
    task_t **pp = &sleep_head;
    while (*pp) {
        task_t *t = *pp;
        if (t->wakeup_tick <= now) {
            *pp = t->wait_next; t->wait_next = NULL;
            sched_unblock(t);
        } else pp = &t->wait_next;
    }
    spin_unlock(&sleep_lock);

    /* Check signals on current task */
    if (cur && cur->signal_pending) {
        if (cur->signal_pending & (1 << SIGKILL)) {
            sched_exit_current(128 + SIGKILL);
            cur->need_resched = 1;
        } else if (cur->signal_pending & (1 << SIGSTOP)) {
            cur->state = TASK_STOPPED;
            cur->signal_pending &= ~(1 << SIGSTOP);
        }
    }

    /* Involuntary preemption disabled from IRQ: yielding here calls context_switch while
       the IST/interrupt stack is active and breaks IRET frames (bogus RIP/CR2 at ~NULL+off). */
    if (cur) cur->invol_switches++;
}

static void timer_isr(interrupt_frame_t *f) { (void)f; apic_eoi(); sched_tick(); }
void sched_install_timer(void) { idt_register_handler(0x20, timer_isr); }
void sched_enable(void)        { sched_enabled = 1; }

void sched_start(void) {
    sched_enabled = 1; sti();
    for (;;) { sched_yield(); hlt(); }
}

void sched_sleep_until(u64 deadline_tick) {
    cli();
    spin_lock(&sleep_lock);
    task_t *cur = this_cpu()->current;
    cur->state = TASK_BLOCKED;
    cur->wakeup_tick = deadline_tick;
    cur->wait_next = sleep_head; sleep_head = cur;
    spin_unlock(&sleep_lock);

    runqueue_t *rq = &per_cpu_rq[0];
    spin_lock(&rq->lock);
    task_t *next = rq_pick_next(rq);
    spin_unlock(&rq->lock);
    if (!next) { sti(); return; }
    switch_to(next);
    sti();
}

/* ---- fork ---- */

/* Child must first appear on the run queue with a valid context_switch tail; the duplicated
 * syscall stack alone is not enough.  Build the same frame shape as sched_spawn, then the
 * trampoline jumps into syscall_exit_from_dispatch with RAX=0. */
static bool sched_fork_arm_child(task_t *child, task_t *parent, u64 syscall_resume_rsp) {
    if (syscall_resume_rsp < (u64)parent->stack ||
        syscall_resume_rsp > (u64)parent->stack + SCHED_STACK_SIZE) {
        kprintf("[SCHED] fork: syscall_resume_rsp outside parent stack\n");
        return false;
    }
    u64 resume = (u64)child->stack + (syscall_resume_rsp - (u64)parent->stack);
    /*
     * Bootstrap frame must lie strictly BELOW the duplicated syscall_entry snapshot (which
     * occupies [syscall_resume_rsp, stack_hi)).  Placing it at stack_hi-72 overwrote the
     * saved user RIP at stack_hi-16 with fork_child_trampoline, so SYSRET faulted in ring 3.
     */
    if (syscall_resume_rsp < (u64)parent->stack + 72) {
        kprintf("[SCHED] fork: syscall_resume_rsp too shallow for bootstrap frame\n");
        return false;
    }
    u64 parent_lo = syscall_resume_rsp - 72;
    u64 *frame = (u64 *)((u64)child->stack + (parent_lo - (u64)parent->stack));

    frame[0] = 0x202;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = 0;
    frame[4] = 0;
    frame[5] = 0;
    frame[6] = 0;
    frame[7] = (u64)fork_child_trampoline;
    frame[8] = resume;

    child->rsp = (u64)frame;
    return true;
}

i64 sys_fork(u64 syscall_resume_rsp) {
    task_t *parent = sched_current();
    if (!parent) return -1;
    if (parent->child_count >= MAX_CHILDREN) return -1;

    if (!task_cache)
        task_cache = kmem_cache_create("task_t", sizeof(task_t), 16);
    task_t *child = (task_t *)kmem_cache_alloc(task_cache);
    if (!child) return -1;

    /* Copy parent state */
    memcpy(child, parent, sizeof(task_t));
    child->id = next_id++;
    child->parent = parent;
    child->child_count = 0;
    memset(child->children, 0, sizeof(child->children));
    child->state = TASK_READY;
    child->exit_code = 0;
    child->signal_pending = 0;
    child->start_tick = this_cpu()->ticks;
    child->utime = child->stime = 0;
    child->vol_switches = child->invol_switches = 0;
    child->slice_ticks = 0;
    child->need_resched = 0;
    child->accounted = 0;
    child->reap_queued = 0;
    child->stack = NULL;
    child->fpu_state = NULL;
    child->pml4 = NULL;
    child->vma_head = NULL;
    child->graph_node = TASK_GRAPH_NODE_INVALID;
    child->next = NULL;
    child->wait_next = NULL;
    child->tls_base = 0;
    child->thread_group = 0;

    /* Allocate new kernel stack */
    child->stack = (u8 *)vmm_alloc_kernel_stack(SCHED_STACK_SIZE);
    if (!child->stack) {
        sched_destroy_task_resources(child, false, false, true);
        return -1;
    }
    memcpy(child->stack, parent->stack, SCHED_STACK_SIZE);

    /* FPU state */
    u32 fork_fpu_sz = cpuid_get_info()->xsave_size;
    if (fork_fpu_sz == 0) fork_fpu_sz = 512;
    child->fpu_state = (u8 *)kmalloc(fork_fpu_sz);
    if (!child->fpu_state) {
        sched_destroy_task_resources(child, false, false, true);
        return -1;
    }
    if (parent->fpu_state) memcpy(child->fpu_state, parent->fpu_state, fork_fpu_sz);
    else memset(child->fpu_state, 0, fork_fpu_sz);

    /* CoW address space */
    if (parent->pml4) {
        child->pml4 = vmm_fork_address_space(parent->pml4);
        if (!child->pml4) {
            sched_destroy_task_resources(child, false, false, true);
            return -1;
        }
    }

    if (!vmm_clone_task_vmas(child, parent)) {
        sched_destroy_task_resources(child, child->pml4 != NULL, false, true);
        return -1;
    }

    child->graph_node = yamgraph_node_create(YAM_NODE_TASK, child->name, child);
    if (child->graph_node == TASK_GRAPH_NODE_INVALID) {
        sched_destroy_task_resources(child, child->pml4 != NULL, false, true);
        return -1;
    }

    if (!sched_fork_arm_child(child, parent, syscall_resume_rsp)) {
        sched_destroy_task_resources(child, child->pml4 != NULL, false, true);
        return -1;
    }

    vfs_task_retain_fds(child);

    parent->children[parent->child_count++] = child;

    /* Add to run queue */
    runqueue_t *rq = &per_cpu_rq[0];
    u64 f = spin_lock_irqsave(&rq->lock);
    child->vruntime = rq->min_vruntime;
    rq_insert(rq, child);
    spin_unlock_irqrestore(&rq->lock, f);
    total_tasks++;
    child->accounted = 1;
    lifetime_tasks_created++;
    lifetime_processes_forked++;

    return (i64)child->id;
}

i64 sched_waitpid(i64 pid, i32 *status, u32 options) {
    task_t *cur = sched_current();
    if (!cur) return -1;

    for (;;) {
        for (u32 i = 0; i < cur->child_count; i++) {
            task_t *child = cur->children[i];
            if (!child) continue;
            if (pid > 0 && (i64)child->id != pid) continue;
            if (child->state == TASK_ZOMBIE || child->state == TASK_DEAD) {
                i32 wait_status = (child->exit_code & 0xFF) << 8;
                if (status) *status = wait_status;
                i64 ret = (i64)child->id;
                sched_drop_task_count(child);
                child->state = TASK_DEAD;
                bool owns_address_space = child->pml4 && child->pml4 != cur->pml4;
                /* Remove from children list */
                cur->children[i] = cur->children[cur->child_count - 1];
                cur->children[cur->child_count - 1] = NULL;
                cur->child_count--;
                lifetime_tasks_reaped++;
                kprintf("[SCHED] reaped child pid=%ld status=0x%x parent=%lu\n",
                        ret, wait_status, cur->id);
                sched_destroy_task_resources(child, owns_address_space, true, true);
                return ret;
            }
        }
        /* No zombie child found — sleep and try again */
        if (options & 1) return 0; /* WNOHANG-compatible first slice */
        task_sleep_ms(10);
    }
}

i64 sys_kill(u64 pid, u32 sig) {
    if (sig >= SIG_MAX) return -1;
    for (int c = 0; c < MAX_CPUS; c++) {
        task_t *active = active_tasks[c];
        if (active && active->id == pid) {
            active->signal_pending |= (1 << sig);
            active->need_resched = 1;
            return 0;
        }
    }
    /* Search all run queues */
    for (int c = 0; c < MAX_CPUS; c++) {
        runqueue_t *rq = &per_cpu_rq[c];
        u64 f = spin_lock_irqsave(&rq->lock);
        for (u32 i = 0; i < rq->count; i++) {
            if (rq->tasks[i]->id == pid) {
                rq->tasks[i]->signal_pending |= (1 << sig);
                spin_unlock_irqrestore(&rq->lock, f);
                return 0;
            }
        }
        spin_unlock_irqrestore(&rq->lock, f);
    }
    return -1;
}

void sched_set_nice(task_t *t, i8 nice) {
    if (nice < MIN_NICE) nice = MIN_NICE;
    if (nice > MAX_NICE) nice = MAX_NICE;
    t->nice = nice;
    t->weight = weight_for_nice(nice);
}

void sched_set_affinity(task_t *t, u64 mask) { t->cpu_affinity = mask; }
u64  sched_get_affinity(task_t *t) { return t->cpu_affinity; }
void sched_set_ai_hint(task_t *t, u8 hint) { t->ai_hint = hint; }

void sched_print_stats(void) {
    kprintf_color(0xFF00DDFF, "\n[SCHED] === Scheduler Statistics ===\n");
    kprintf("  Total tasks: %lu\n", total_tasks);
    kprintf("  Lifetime: created=%lu forked=%lu threads=%lu reaped=%lu freed_tasks=%lu\n",
            lifetime_tasks_created, lifetime_processes_forked,
            lifetime_threads_created, lifetime_tasks_reaped,
            lifetime_task_objects_freed);
    kprintf("  Cleanup: stacks=%lu pml4=%lu vmas=%lu fd_tables=%lu graph_nodes=%lu\n",
            lifetime_kernel_stacks_freed, lifetime_user_pml4s_destroyed,
            lifetime_vma_lists_destroyed, lifetime_fd_tables_closed,
            lifetime_graph_nodes_destroyed);
    kprintf("  Orphans: detached=%lu detached_reaped=%lu\n",
            lifetime_orphans_detached, lifetime_detached_tasks_reaped);
    for (int c = 0; c < MAX_CPUS; c++) {
        if (per_cpu_rq[c].count > 0 || c == 0) {
            kprintf("  CPU%d: %u ready, min_vrt=%lu\n",
                    c, per_cpu_rq[c].count, per_cpu_rq[c].min_vruntime);
        }
    }
}

void sched_get_info(sched_info_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->detected_cpus = smp_cpu_count();
    out->schedulable_cpus = smp_sched_cpu_count();
    out->total_tasks = (u32)total_tasks;
    out->ticks = this_cpu()->ticks;
    out->lifetime_tasks_created = lifetime_tasks_created;
    out->lifetime_processes_forked = lifetime_processes_forked;
    out->lifetime_threads_created = lifetime_threads_created;
    out->lifetime_tasks_reaped = lifetime_tasks_reaped;
    out->lifetime_task_objects_freed = lifetime_task_objects_freed;
    out->lifetime_kernel_stacks_freed = lifetime_kernel_stacks_freed;
    out->lifetime_user_pml4s_destroyed = lifetime_user_pml4s_destroyed;
    out->lifetime_vma_lists_destroyed = lifetime_vma_lists_destroyed;
    out->lifetime_fd_tables_closed = lifetime_fd_tables_closed;
    out->lifetime_graph_nodes_destroyed = lifetime_graph_nodes_destroyed;
    out->lifetime_orphans_detached = lifetime_orphans_detached;
    out->lifetime_detached_tasks_reaped = lifetime_detached_tasks_reaped;

    for (int c = 0; c < MAX_CPUS; c++) {
        runqueue_t *rq = &per_cpu_rq[c];
        u64 f = spin_lock_irqsave(&rq->lock);
        out->rq_ready[c] = rq->count;
        out->rq_load[c] = rq->load_weight;
        out->ready_tasks += rq->count;
        out->total_switches += rq->nr_switches;
        spin_unlock_irqrestore(&rq->lock, f);

        task_t *active = active_tasks[c];
        if (active && active->state == TASK_RUNNING) out->running_tasks++;
    }
}

/* ---- Kernel threads (User-space threads) ---- */
task_t *sched_thread_create(const char *name, u64 entry, u64 stack_top, u64 arg, u64 tls_base) {
    task_t *parent = sched_current();
    if (!parent) return NULL;
    if (parent->child_count >= MAX_CHILDREN) return NULL;

    if (!task_cache)
        task_cache = kmem_cache_create("task_t", sizeof(task_t), 16);
    task_t *child = (task_t *)kmem_cache_alloc(task_cache);
    if (!child) return NULL;

    /* Copy parent state */
    memcpy(child, parent, sizeof(task_t));
    child->id = next_id++;
    child->parent = parent;
    child->child_count = 0;
    memset(child->children, 0, sizeof(child->children));
    child->state = TASK_READY;
    child->exit_code = 0;
    child->signal_pending = 0;
    child->start_tick = this_cpu()->ticks;
    child->utime = child->stime = 0;
    child->vol_switches = child->invol_switches = 0;
    child->slice_ticks = 0;
    child->need_resched = 0;
    child->accounted = 0;
    child->reap_queued = 0;
    child->tls_base = tls_base;
    child->thread_group = parent->thread_group ? parent->thread_group : parent->id;
    child->stack = NULL;
    child->fpu_state = NULL;
    child->graph_node = TASK_GRAPH_NODE_INVALID;
    child->next = NULL;
    child->wait_next = NULL;

    /* Give it a name */
    if (name) {
        memset(child->name, 0, sizeof(child->name));
        for (u32 i = 0; i < sizeof(child->name) - 1 && name[i]; i++) child->name[i] = name[i];
    }
    /* Allocate new kernel stack */
    child->stack = (u8 *)vmm_alloc_kernel_stack(SCHED_STACK_SIZE);
    if (!child->stack) {
        sched_destroy_task_resources(child, false, false, true);
        return NULL;
    }
    memset(child->stack, 0, SCHED_STACK_SIZE);

    /* FPU state */
    u32 fork_fpu_sz = cpuid_get_info()->xsave_size;
    if (fork_fpu_sz == 0) fork_fpu_sz = 512;
    child->fpu_state = (u8 *)kmalloc(fork_fpu_sz);
    if (!child->fpu_state) {
        sched_destroy_task_resources(child, false, false, true);
        return NULL;
    }
    if (parent->fpu_state) memcpy(child->fpu_state, parent->fpu_state, fork_fpu_sz);
    else memset(child->fpu_state, 0, fork_fpu_sz);

    /* Share address space (no CoW) */
    child->pml4 = parent->pml4;

    child->graph_node = yamgraph_node_create(YAM_NODE_TASK, child->name, child);
    if (child->graph_node == TASK_GRAPH_NODE_INVALID) {
        sched_destroy_task_resources(child, false, false, true);
        return NULL;
    }

    /* Set up stack frame for enter_user_mode */
    /* We don't use task_trampoline for this, we push an iretq frame directly. */
    u64 *sp = (u64 *)(child->stack + SCHED_STACK_SIZE);
    
    *--sp = 0x1B;               /* SS = user data | RPL3 */
    *--sp = stack_top;          /* user RSP */
    *--sp = 0x202;              /* RFLAGS */
    *--sp = 0x23;               /* CS = user code | RPL3 */
    *--sp = entry;              /* user RIP */

    /* Context switch frame */
    extern void context_switch_user_thread_stub(void);
    *--sp = arg;                /* Will be popped into rdi by the stub */
    *--sp = (u64)context_switch_user_thread_stub; /* Return address for context_switch */
    *--sp = 0;                  /* rbp */
    *--sp = 0;                  /* rbx */
    *--sp = 0;                  /* r12 */
    *--sp = 0;                  /* r13 */
    *--sp = 0;                  /* r14 */
    *--sp = 0;                  /* r15 */
    *--sp = 0x202;              /* rflags (for context_switch's popfq) */
    
    child->rsp = (u64)sp;

    vfs_task_retain_fds(child);

    parent->children[parent->child_count++] = child;

    /* Add to run queue */
    runqueue_t *rq = &per_cpu_rq[0];
    u64 f = spin_lock_irqsave(&rq->lock);
    child->vruntime = rq->min_vruntime;
    rq_insert(rq, child);
    spin_unlock_irqrestore(&rq->lock, f);
    total_tasks++;
    child->accounted = 1;
    lifetime_tasks_created++;
    lifetime_threads_created++;

    return child;
}

