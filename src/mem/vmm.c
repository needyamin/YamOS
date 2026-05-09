/* ============================================================================
 * YamKernel — Virtual Memory Manager Implementation
 * v0.3.0: CoW, huge pages, mprotect, brk, VMA merge/split
 * 4-level page tables (PML4 → PDPT → PD → PT) for x86_64
 * ============================================================================ */

#include "vmm.h"
#include "pmm.h"
#include "../cpu/apic.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"

static u64 g_hhdm_offset = 0;
static u64 g_next_kernel_stack = 0xFFFFE10000000000ULL;
/** Canonical kernel page-table root captured once during vmm_init (BSP CR3).
 * Never derive kernel mappings by reading CR3 during scheduling — CR3 may still
 * hold a user address space when we're deciding whether to reload CR3 for a
 * kernel thread whose task_t.pml4 is NULL (fixes wedge after fork/exec bursts).
 */
static u64 *g_kernel_pml4 = NULL;
#define USER_BRK_DEFAULT 0x400000ULL

/* ---- Helpers ---- */
void *vmm_phys_to_virt(u64 phys) { return (void *)(phys + g_hhdm_offset); }
u64 vmm_virt_hhdm_to_phys(void *virt) { return (u64)virt - g_hhdm_offset; }

static inline u64 pml4_index(u64 virt) { return (virt >> 39) & 0x1FF; }
static inline u64 pdpt_index(u64 virt) { return (virt >> 30) & 0x1FF; }
static inline u64 pd_index(u64 virt)   { return (virt >> 21) & 0x1FF; }
static inline u64 pt_index(u64 virt)   { return (virt >> 12) & 0x1FF; }

static u64 *alloc_page_table(void) {
    u64 phys = pmm_alloc_page();
    if (!phys) return NULL;
    u64 *virt = (u64 *)vmm_phys_to_virt(phys);
    memset(virt, 0, PAGE_SIZE);
    return virt;
}

static u64 *get_or_create_table(u64 *table, u64 index, u64 flags) {
    if (table[index] & VMM_FLAG_PRESENT) {
        if (table[index] & (1ULL << 7)) return NULL; /* Huge page */
        u64 phys = table[index] & 0x000FFFFFFFFFF000ULL;
        return (u64 *)vmm_phys_to_virt(phys);
    }
    u64 *new_table = alloc_page_table();
    if (!new_table) return NULL;
    u64 phys = vmm_virt_hhdm_to_phys(new_table);
    table[index] = phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE | flags;
    return new_table;
}

/* ---- TLB ---- */
void vmm_flush_tlb_page(u64 virt) {
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

void vmm_flush_tlb_all(void) {
    u64 cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

void vmm_shootdown_page(u64 virt) {
    vmm_flush_tlb_page(virt);
    apic_broadcast_tlb_shootdown(virt, false);
}

void vmm_shootdown_all(void) {
    vmm_flush_tlb_all();
    apic_broadcast_tlb_shootdown(0, true);
}

void *vmm_alloc_kernel_stack(usize usable_size) {
    if (usable_size == 0) return NULL;
    usable_size = (usable_size + PAGE_SIZE - 1) & ~(usize)(PAGE_SIZE - 1);

    u64 total = usable_size + 2 * PAGE_SIZE;
    u64 base = __atomic_fetch_add(&g_next_kernel_stack, total, __ATOMIC_RELAXED);
    u64 usable = base + PAGE_SIZE;
    u64 *pml4 = vmm_get_kernel_pml4();

    for (u64 off = 0; off < usable_size; off += PAGE_SIZE) {
        u64 phys = pmm_alloc_page();
        if (!phys) {
            vmm_free_kernel_stack((void *)usable, usable_size);
            return NULL;
        }
        if (!vmm_map_page(pml4, usable + off, phys,
                          VMM_FLAG_WRITE | VMM_FLAG_NX)) {
            pmm_free_page(phys);
            vmm_free_kernel_stack((void *)usable, usable_size);
            return NULL;
        }
    }

    kprintf("[VMM] guarded kernel stack usable=0x%lx size=%lu guard_lo=0x%lx guard_hi=0x%lx\n",
            usable, usable_size, base, usable + usable_size);
    return (void *)usable;
}

void vmm_free_kernel_stack(void *stack, usize usable_size) {
    if (!stack || usable_size == 0) return;
    usable_size = (usable_size + PAGE_SIZE - 1) & ~(usize)(PAGE_SIZE - 1);

    u64 usable = (u64)stack;
    u64 *pml4 = vmm_get_kernel_pml4();
    for (u64 off = 0; off < usable_size; off += PAGE_SIZE) {
        u64 va = usable + off;
        u64 phys = vmm_virt_to_phys(pml4, va);
        if (!phys) continue;
        vmm_unmap_page(pml4, va);
        pmm_free_page(phys & ~0xFFFULL);
    }
}

/* ---- Public API ---- */
void vmm_init(u64 hhdm_offset) {
    g_hhdm_offset = hhdm_offset;
    u64 pml4_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(pml4_phys));
    g_kernel_pml4 =
        (u64 *)vmm_phys_to_virt(pml4_phys & 0x000FFFFFFFFFF000ULL);
    kprintf_color(0xFF00FF88, "[VMM] Initialized: HHDM offset = 0x%lx\n",
                  hhdm_offset);
}

bool vmm_map_page(u64 *pml4, u64 virt, u64 phys, u64 flags) {
    u64 *pdpt = get_or_create_table(pml4, pml4_index(virt), flags & VMM_FLAG_USER);
    if (!pdpt) return false;
    u64 *pd = get_or_create_table(pdpt, pdpt_index(virt), flags & VMM_FLAG_USER);
    if (!pd) return false;
    u64 *pt = get_or_create_table(pd, pd_index(virt), flags & VMM_FLAG_USER);
    if (!pt) return false;
    pt[pt_index(virt)] = (phys & 0x000FFFFFFFFFF000ULL) | flags | VMM_FLAG_PRESENT;
    vmm_shootdown_page(virt);
    return true;
}

bool vmm_map_huge_page(u64 *pml4, u64 virt, u64 phys, u64 flags) {
    /* 2MB huge page: mapped at PD level with PS bit */
    if ((virt & (HUGE_PAGE_SIZE - 1)) || (phys & (HUGE_PAGE_SIZE - 1))) return false;
    u64 *pdpt = get_or_create_table(pml4, pml4_index(virt), flags & VMM_FLAG_USER);
    if (!pdpt) return false;
    u64 *pd = get_or_create_table(pdpt, pdpt_index(virt), flags & VMM_FLAG_USER);
    if (!pd) return false;
    pd[pd_index(virt)] = (phys & 0x000FFFFFFFE00000ULL) | flags | VMM_FLAG_PRESENT | VMM_FLAG_HUGE;
    vmm_shootdown_page(virt);
    return true;
}

void vmm_unmap_page(u64 *pml4, u64 virt) {
    if (!(pml4[pml4_index(virt)] & VMM_FLAG_PRESENT)) return;
    u64 *pdpt = (u64 *)vmm_phys_to_virt(pml4[pml4_index(virt)] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[pdpt_index(virt)] & VMM_FLAG_PRESENT)) return;
    u64 *pd = (u64 *)vmm_phys_to_virt(pdpt[pdpt_index(virt)] & 0x000FFFFFFFFFF000ULL);
    if (!(pd[pd_index(virt)] & VMM_FLAG_PRESENT)) return;
    u64 *pt = (u64 *)vmm_phys_to_virt(pd[pd_index(virt)] & 0x000FFFFFFFFFF000ULL);
    pt[pt_index(virt)] = 0;
    vmm_shootdown_page(virt);
}

u64 vmm_virt_to_phys(u64 *pml4, u64 virt) {
    if (!(pml4[pml4_index(virt)] & VMM_FLAG_PRESENT)) return 0;
    u64 *pdpt = (u64 *)vmm_phys_to_virt(pml4[pml4_index(virt)] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[pdpt_index(virt)] & VMM_FLAG_PRESENT)) return 0;
    u64 *pd = (u64 *)vmm_phys_to_virt(pdpt[pdpt_index(virt)] & 0x000FFFFFFFFFF000ULL);
    if (!(pd[pd_index(virt)] & VMM_FLAG_PRESENT)) return 0;
    /* Check for 2MB huge page */
    if (pd[pd_index(virt)] & VMM_FLAG_HUGE) {
        return (pd[pd_index(virt)] & 0x000FFFFFFFE00000ULL) | (virt & (HUGE_PAGE_SIZE - 1));
    }
    u64 *pt = (u64 *)vmm_phys_to_virt(pd[pd_index(virt)] & 0x000FFFFFFFFFF000ULL);
    if (!(pt[pt_index(virt)] & VMM_FLAG_PRESENT)) return 0;
    return (pt[pt_index(virt)] & 0x000FFFFFFFFFF000ULL) | (virt & 0xFFF);
}

u64 vmm_get_pte_flags(u64 *pml4, u64 virt) {
    if (!(pml4[pml4_index(virt)] & VMM_FLAG_PRESENT)) return 0;
    u64 *pdpt = (u64 *)vmm_phys_to_virt(pml4[pml4_index(virt)] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[pdpt_index(virt)] & VMM_FLAG_PRESENT)) return 0;
    u64 *pd = (u64 *)vmm_phys_to_virt(pdpt[pdpt_index(virt)] & 0x000FFFFFFFFFF000ULL);
    if (!(pd[pd_index(virt)] & VMM_FLAG_PRESENT)) return 0;
    if (pd[pd_index(virt)] & VMM_FLAG_HUGE) return pd[pd_index(virt)] & 0xFFF0000000000FFFULL;
    u64 *pt = (u64 *)vmm_phys_to_virt(pd[pd_index(virt)] & 0x000FFFFFFFFFF000ULL);
    return pt[pt_index(virt)] & 0xFFF0000000000FFFULL;
}

bool vmm_update_flags(u64 *pml4, u64 virt, u64 new_flags) {
    if (!(pml4[pml4_index(virt)] & VMM_FLAG_PRESENT)) return false;
    u64 *pdpt = (u64 *)vmm_phys_to_virt(pml4[pml4_index(virt)] & 0x000FFFFFFFFFF000ULL);
    if (!(pdpt[pdpt_index(virt)] & VMM_FLAG_PRESENT)) return false;
    u64 *pd = (u64 *)vmm_phys_to_virt(pdpt[pdpt_index(virt)] & 0x000FFFFFFFFFF000ULL);
    if (!(pd[pd_index(virt)] & VMM_FLAG_PRESENT)) return false;
    if (pd[pd_index(virt)] & VMM_FLAG_HUGE) {
        u64 phys = pd[pd_index(virt)] & 0x000FFFFFFFE00000ULL;
        pd[pd_index(virt)] = phys | new_flags | VMM_FLAG_PRESENT | VMM_FLAG_HUGE;
        vmm_shootdown_page(virt);
        return true;
    }
    u64 *pt = (u64 *)vmm_phys_to_virt(pd[pd_index(virt)] & 0x000FFFFFFFFFF000ULL);
    if (!(pt[pt_index(virt)] & VMM_FLAG_PRESENT)) return false;
    u64 phys = pt[pt_index(virt)] & 0x000FFFFFFFFFF000ULL;
    pt[pt_index(virt)] = phys | new_flags | VMM_FLAG_PRESENT;
    vmm_shootdown_page(virt);
    return true;
}

u64 *vmm_get_kernel_pml4(void) {
    if (g_kernel_pml4)
        return g_kernel_pml4;
    u64 pml4_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(pml4_phys));
    return (u64 *)vmm_phys_to_virt(pml4_phys & 0x000FFFFFFFFFF000ULL);
}

u64 *vmm_create_user_pml4(void) {
    u64 *new_pml4 = alloc_page_table();
    if (!new_pml4) return NULL;
    u64 *kernel_pml4 = vmm_get_kernel_pml4();
    for (int i = 256; i < 512; i++) new_pml4[i] = kernel_pml4[i];
    return new_pml4;
}

/* ---- CoW: Fork address space ---- */
u64 *vmm_fork_address_space(u64 *src_pml4) {
    u64 *new_pml4 = vmm_create_user_pml4();
    if (!new_pml4) return NULL;

    /* Walk user half (entries 0-255) and share pages as CoW */
    for (int i = 0; i < 256; i++) {
        if (!(src_pml4[i] & VMM_FLAG_PRESENT)) continue;
        u64 *src_pdpt = (u64 *)vmm_phys_to_virt(src_pml4[i] & 0x000FFFFFFFFFF000ULL);
        u64 *dst_pdpt = alloc_page_table();
        if (!dst_pdpt) continue;
        new_pml4[i] = vmm_virt_hhdm_to_phys(dst_pdpt) | (src_pml4[i] & 0xFFF);

        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt[j] & VMM_FLAG_PRESENT)) continue;
            u64 *src_pd = (u64 *)vmm_phys_to_virt(src_pdpt[j] & 0x000FFFFFFFFFF000ULL);
            u64 *dst_pd = alloc_page_table();
            if (!dst_pd) continue;
            dst_pdpt[j] = vmm_virt_hhdm_to_phys(dst_pd) | (src_pdpt[j] & 0xFFF);

            for (int k = 0; k < 512; k++) {
                if (!(src_pd[k] & VMM_FLAG_PRESENT)) continue;
                if (src_pd[k] & VMM_FLAG_HUGE) {
                    /* Huge pages: share directly, mark CoW */
                    u64 phys = src_pd[k] & 0x000FFFFFFFE00000ULL;
                    u64 flags = src_pd[k] & 0xFFF;
                    flags &= ~VMM_FLAG_WRITE;
                    flags |= VMM_FLAG_COW;
                    dst_pd[k] = phys | flags | VMM_FLAG_HUGE;
                    src_pd[k] = phys | flags | VMM_FLAG_HUGE;
                    pmm_page_ref(phys);
                    continue;
                }
                u64 *src_pt = (u64 *)vmm_phys_to_virt(src_pd[k] & 0x000FFFFFFFFFF000ULL);
                u64 *dst_pt = alloc_page_table();
                if (!dst_pt) continue;
                dst_pd[k] = vmm_virt_hhdm_to_phys(dst_pt) | (src_pd[k] & 0xFFF);

                for (int l = 0; l < 512; l++) {
                    if (!(src_pt[l] & VMM_FLAG_PRESENT)) continue;
                    u64 phys = src_pt[l] & 0x000FFFFFFFFFF000ULL;
                    u64 flags = src_pt[l] & 0xFFF0000000000FFFULL;
                    if (!(flags & VMM_FLAG_DONT_FREE)) {
                        /* Mark both parent and child as CoW (read-only + CoW bit) */
                        flags &= ~VMM_FLAG_WRITE;
                        flags |= VMM_FLAG_COW;
                        src_pt[l] = phys | flags;
                        pmm_page_ref(phys);
                    }
                    dst_pt[l] = phys | flags;
                }
            }
        }
    }
    vmm_shootdown_all();
    return new_pml4;
}

/* ---- CoW fault handler ---- */
bool vmm_handle_cow_fault(u64 *pml4, u64 fault_addr) {
    u64 page_addr = fault_addr & ~0xFFFULL;
    u64 flags = vmm_get_pte_flags(pml4, page_addr);
    if (!(flags & VMM_FLAG_COW)) return false;

    u64 old_phys = vmm_virt_to_phys(pml4, page_addr) & ~0xFFFULL;
    if (!old_phys) return false;

    u32 rc = pmm_page_refcount(old_phys);
    if (rc <= 1) {
        /* Last reference — just make it writable again */
        u64 new_flags = (flags & ~VMM_FLAG_COW) | VMM_FLAG_WRITE;
        vmm_update_flags(pml4, page_addr, new_flags);
        return true;
    }

    /* Multiple references — copy the page */
    u64 new_phys = pmm_alloc_page();
    if (!new_phys) return false;

    void *src = vmm_phys_to_virt(old_phys);
    void *dst = vmm_phys_to_virt(new_phys);
    memcpy(dst, src, PAGE_SIZE);

    /* Map new page as writable, drop CoW bit */
    u64 new_flags = (flags & ~VMM_FLAG_COW) | VMM_FLAG_WRITE;
    vmm_map_page(pml4, page_addr, new_phys, new_flags);

    /* Drop reference on old page */
    pmm_page_unref(old_phys);
    return true;
}

static void release_user_mapping(u64 entry, bool huge) {
    if (!(entry & VMM_FLAG_PRESENT)) return;
    if (entry & VMM_FLAG_DONT_FREE) return;

    u64 phys = entry & (huge ? 0x000FFFFFFFE00000ULL : 0x000FFFFFFFFFF000ULL);
    if (!phys) return;
    pmm_page_unref(phys);
}

static inline u64 page_align_up(u64 x) {
    return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static void free_pt(u64 *pt) {
    for (int i = 0; i < 512; i++) {
        release_user_mapping(pt[i], false);
    }
}

static void free_pd(u64 *pd) {
    for (int i = 0; i < 512; i++) {
        if (!(pd[i] & VMM_FLAG_PRESENT)) continue;

        u64 phys = pd[i] & 0x000FFFFFFFFFF000ULL;
        if (pd[i] & VMM_FLAG_HUGE) {
            release_user_mapping(pd[i], true);
            continue;
        }

        u64 *pt = (u64 *)vmm_phys_to_virt(phys);
        free_pt(pt);
        pmm_free_page(phys);
    }
}

static void free_pdpt(u64 *pdpt) {
    for (int i = 0; i < 512; i++) {
        if (!(pdpt[i] & VMM_FLAG_PRESENT)) continue;

        u64 phys = pdpt[i] & 0x000FFFFFFFFFF000ULL;
        u64 *pd = (u64 *)vmm_phys_to_virt(phys);
        free_pd(pd);
        pmm_free_page(phys);
    }
}

void vmm_destroy_user_pml4(u64 *pml4) {
    if (!pml4) return;
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & VMM_FLAG_PRESENT)) continue;

        u64 phys = pml4[i] & 0x000FFFFFFFFFF000ULL;
        u64 *pdpt = (u64 *)vmm_phys_to_virt(phys);
        free_pdpt(pdpt);
        pmm_free_page(phys);
    }
    pmm_free_page(vmm_virt_hhdm_to_phys(pml4));
}

/* ---- mmap subsystem ---- */
#include "../sched/sched.h"
#include "../mem/heap.h"

#define MMAP_BASE   0x7F0000000000ULL
static u64 next_mmap_addr = MMAP_BASE;

void *sys_mmap(void *addr, usize length, u32 prot, u32 flags, int fd, usize offset) {
    (void)addr; (void)fd; (void)offset;
    if (length == 0) return MAP_FAILED;

    usize pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    usize size  = pages * PAGE_SIZE;

    u64 vaddr = next_mmap_addr;
    next_mmap_addr += size;

    vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t));
    if (!vma) return MAP_FAILED;
    memset(vma, 0, sizeof(*vma));
    vma->start = vaddr;
    vma->end   = vaddr + size;
    vma->flags = flags;
    vma->prot  = prot;
    vma->file  = NULL;

    task_t *t = sched_current();
    if (t) {
        vma->next = (vma_t *)t->vma_head;
        t->vma_head = (struct vma *)vma;
    }

    if (!(flags & MAP_ANONYMOUS)) {
        for (usize i = 0; i < pages; i++) {
            u64 phys = pmm_alloc_page();
            if (!phys) return MAP_FAILED;
            u64 pflags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER;
            if (!(prot & PROT_EXEC)) pflags |= VMM_FLAG_NX;
            vmm_map_page(t->pml4, vaddr + i * PAGE_SIZE, phys, pflags);
            void *mapped = vmm_phys_to_virt(phys);
            memset(mapped, 0, PAGE_SIZE);
        }
    }
    return (void *)vaddr;
}

int sys_munmap(void *addr, usize length) {
    u64 vaddr = (u64)addr;
    usize pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    task_t *t = sched_current();
    if (!t || !t->pml4) return -1;

    u64 *pml4 = t->pml4;
    for (usize i = 0; i < pages; i++) {
        u64 va = vaddr + i * PAGE_SIZE;
        u64 phys = vmm_virt_to_phys(pml4, va);
        if (phys) {
            vmm_unmap_page(pml4, va);
            pmm_free_page(phys & ~0xFFFULL);
        }
    }

    if (t) {
        vma_t **prev = (vma_t **)&t->vma_head;
        for (vma_t *v = (vma_t *)t->vma_head; v; v = v->next) {
            if (v->start == vaddr) {
                *prev = v->next;
                kfree(v);
                break;
            }
            prev = &v->next;
        }
    }
    return 0;
}

int sys_mprotect(void *addr, usize length, u32 prot) {
    u64 vaddr = (u64)addr & ~0xFFFULL;
    usize pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    task_t *t = sched_current();
    if (!t || !t->pml4) return -1;

    for (usize i = 0; i < pages; i++) {
        u64 va = vaddr + i * PAGE_SIZE;
        u64 old_flags = vmm_get_pte_flags(t->pml4, va);
        if (!(old_flags & VMM_FLAG_PRESENT)) continue;

        u64 new_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (prot & PROT_WRITE) new_flags |= VMM_FLAG_WRITE;
        if (!(prot & PROT_EXEC)) new_flags |= VMM_FLAG_NX;
        /* Preserve OS-defined bits */
        if (old_flags & VMM_FLAG_COW) new_flags |= VMM_FLAG_COW;
        if (old_flags & VMM_FLAG_DONT_FREE) new_flags |= VMM_FLAG_DONT_FREE;

        vmm_update_flags(t->pml4, va, new_flags);
    }

    /* Update VMA prot */
    for (vma_t *v = (vma_t *)t->vma_head; v; v = v->next) {
        if (v->start <= vaddr && v->end >= vaddr + pages * PAGE_SIZE) {
            v->prot = prot;
            break;
        }
    }
    return 0;
}

u64 sys_brk(u64 new_brk) {
    task_t *t = sched_current();
    if (!t) return 0;

    if (t->brk_start == 0) {
        t->brk_start = USER_BRK_DEFAULT;
        t->brk_current = USER_BRK_DEFAULT;
    }
    if (new_brk == 0) return t->brk_current;
    if (new_brk < t->brk_start) return t->brk_current;

    u64 old_brk = t->brk_current;
    if (new_brk > old_brk) {
        u64 start = page_align_up(old_brk);
        u64 end = page_align_up(new_brk);
        if (t->pml4) {
            for (u64 va = start; va < end; va += PAGE_SIZE) {
                if (vmm_virt_to_phys(t->pml4, va)) continue;
                u64 phys = pmm_alloc_page();
                if (!phys) return t->brk_current;
                vmm_map_page(t->pml4, va, phys,
                    VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NX);
                memset(vmm_phys_to_virt(phys), 0, PAGE_SIZE);
            }
        }
    } else if (new_brk < old_brk && t->pml4) {
        u64 start = page_align_up(new_brk);
        u64 end = page_align_up(old_brk);
        for (u64 va = start; va < end; va += PAGE_SIZE) {
            u64 phys = vmm_virt_to_phys(t->pml4, va);
            if (!phys) continue;
            vmm_unmap_page(t->pml4, va);
            pmm_page_unref(phys & ~0xFFFULL);
        }
    }
    t->brk_current = new_brk;
    return t->brk_current;
}

void vmm_destroy_task_vmas(task_t *t) {
    if (!t) return;
    vma_t *v = (vma_t *)t->vma_head;
    while (v) {
        vma_t *next = v->next;
        kfree(v);
        v = next;
    }
    t->vma_head = NULL;
}

bool vmm_clone_task_vmas(task_t *dst, const task_t *src) {
    if (!dst || !src) return false;
    dst->vma_head = NULL;

    vma_t **tail = (vma_t **)&dst->vma_head;
    for (const vma_t *v = (const vma_t *)src->vma_head; v; v = v->next) {
        vma_t *copy = (vma_t *)kmalloc(sizeof(*copy));
        if (!copy) {
            vmm_destroy_task_vmas(dst);
            return false;
        }
        memcpy(copy, v, sizeof(*copy));
        copy->next = NULL;
        *tail = copy;
        tail = &copy->next;
    }
    return true;
}

/* ---- Demand Paging ---- */
bool vmm_handle_page_fault(u64 fault_addr, u64 error_code) {
    task_t *t = sched_current();
    if (!t) return false;

    /* Check for CoW fault first (write to read-only CoW page) */
    if ((error_code & 0x03) == 0x03 && t->pml4) { /* Present + Write */
        if (vmm_handle_cow_fault(t->pml4, fault_addr)) return true;
    }

    /* Search VMA list for the faulting address */
    for (vma_t *v = (vma_t *)t->vma_head; v; v = v->next) {
        if (fault_addr >= v->start && fault_addr < v->end) {
            u64 page_addr = fault_addr & ~0xFFFULL;
            u64 phys = pmm_alloc_page();
            if (!phys) return false;

            u64 pflags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
            if (v->prot & PROT_WRITE) pflags |= VMM_FLAG_WRITE;
            if (!(v->prot & PROT_EXEC)) pflags |= VMM_FLAG_NX;

            vmm_map_page(t->pml4, page_addr, phys, pflags);
            void *mapped = vmm_phys_to_virt(phys);
            memset(mapped, 0, PAGE_SIZE);
            return true;
        }
    }
    return false;
}
