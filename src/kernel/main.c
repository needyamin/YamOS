/* ============================================================================
 * YamKernel — Main Entry Point
 * 
 * This is where everything begins after the Limine bootloader hands off
 * control to the kernel in 64-bit long mode.
 *
 * YamKernel — A Graph-Based Adaptive Operating System
 * Copyright (C) 2026  
 * ============================================================================ */

#include <nexus/types.h>
#include <nexus/panic.h>
#include <limine.h>

#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/cpuid.h"
#include "cpu/security.h"
#include "cpu/acpi.h"
#include "cpu/apic.h"
#include "cpu/percpu.h"
#include "cpu/smp.h"
#include "cpu/tsc.h"
#include "kernel/api/syscall.h"
#include "sched/sched.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "mem/heap.h"
#include "drivers/serial/serial.h"
#include "drivers/video/framebuffer.h"
#include "drivers/timer/pit.h"
#include "drivers/timer/hpet.h"
#include "drivers/timer/rtc.h"
#include "drivers/bus/api.h"
#include "net/net.h"
#include "ipc/ipc.h"
#include "fs/vfs.h"
#include "fs/bcache.h"
#include "fs/elf.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "lib/kdebug.h"
#include "nexus/graph.h"
#include "nexus/capability.h"
#include "os/services/compositor/compositor.h"
#include "nexus/channel.h"
#include "drivers/bus/pci.h"
#include "drivers/core/driver_manager.h"
#include "drivers/block/block.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/block/ahci.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/mouse.h"
#include "drivers/input/evdev.h"
#include "drivers/drm/drm.h"
#include "sched/wait.h"
#include "sched/cgroup.h"
#include "kernel/shell.h"
#include "boot/yamboot.h"
#include "mem/oom.h"
#include "cpu/power.h"
#include "drivers/ai/ai_accel.h"
#include "drivers/input/touch.h"
#include "drivers/input/gesture.h"
#include "drivers/audio/audio.h"
#include "os/services/installer/installer.h"
#include "os/services/app_registry/app_registry.h"

#define YAM_DEMO_TASKS 0
#define YAM_PREEMPTIVE 1
#define YAM_WAYLAND    1

/* ============================================================================
 * Limine Requests — the bootloader fills these in before calling us
 * ============================================================================ */

LIMINE_BASE_REVISION(0)

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0  /* 0 = default (x2APIC enabled if available) */
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_address_request kaddr_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

/* ============================================================================
 * Global module pointers
 * ============================================================================ */
void *g_authd_module = NULL;  usize g_authd_module_size = 0;
void *g_hello_module = NULL;  usize g_hello_module_size = 0;
void *g_exec_test_module = NULL; usize g_exec_test_module_size = 0;
void *g_orphan_test_module = NULL; usize g_orphan_test_module_size = 0;
void *g_wallpaper_module = NULL;

/* ============================================================================
 * Boot Banner
 * ============================================================================ */

static void print_banner(void) {
    kprintf_color(0xFF00DDFF,
        "\n"
        "  ██╗   ██╗ █████╗ ███╗   ███╗\n"
        "  ╚██╗ ██╔╝██╔══██╗████╗ ████║\n"
        "   ╚████╔╝ ███████║██╔████╔██║\n"
        "    ╚██╔╝  ██╔══██║██║╚██╔╝██║\n"
        "     ██║   ██║  ██║██║ ╚═╝ ██║\n"
        "     ╚═╝   ╚═╝  ╚═╝╚═╝     ╚═╝\n"
    );
    kprintf_color(0xFFFFDD00,
        "  ╦╔═╔═╗╦═╗╔╗╔╔═╗╦  \n"
        "  ╠╩╗║╣ ╠╦╝║║║║╣ ║  \n"
        "  ╩ ╩╚═╝╩╚═╝╚╝╚═╝╩═╝\n"
    );
    kprintf_color(0xFF00FF88,
        "\n  YamOS Kernel v0.3.0 — Next-Gen Adaptive Operating System\n");
    kprintf_color(0xFF888888,
        "  Architecture: x86_64 | Model: YamGraph Resource Graph\n"
        "  Features: Zone-PMM CoW CFS AI Touch cgroups OOM Power\n"
        "  Built: " __DATE__ " " __TIME__ "\n\n");
}

/* ============================================================================
 * Kernel Main
 * ============================================================================ */

/* Entry point — called by Limine bootloader */
void kernel_main(void) {
    /* ---- Phase 1: Early console (serial only) ---- */
    serial_init();
    KINFO("BOOT", "=== YamOS Boot Start ===");
    KINFO("BOOT", "Serial console initialized");

    /* ---- Phase 2: Validate bootloader response ---- */
    if (limine_base_revision[2] != 0) {
        KERR("BOOT", "Limine base revision mismatch! Halting.");
        for (;;) hlt();
    }
    KINFO("BOOT", "Limine base revision OK");

    /* ---- Phase 3: Framebuffer ---- */
    if (fb_request.response && fb_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
        fb_init(fb);
        KINFO("FB", "Framebuffer: %lux%lu @ %u bpp, addr=%p, pitch=%lu",
              fb->width, fb->height, fb->bpp, fb->address, fb->pitch);
    } else {
        KWARN("FB", "No framebuffer available!");
    }

    /* ---- YamBoot: custom boot menu (polled, no IDT yet) ---- */
    KINFO("BOOT", "Showing YamBoot menu...");
    yamboot_choice_t choice = yamboot_show();
    if (choice == YAMBOOT_REBOOT) {
        KINFO("BOOT", "User chose REBOOT");
        outb(0x64, 0xFE);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    KINFO("BOOT", "User chose boot mode %d (safe=%d)", (int)choice, g_yamboot_safe);

    /* ---- Draw Windows-Style Splash Screen AFTER YamBoot ---- */
    KINFO("SPLASH", "--- Splash Screen Init ---");
    if (fb_request.response && fb_request.response->framebuffer_count > 0) {
        void *wallpaper_data = NULL;
        void *logo_data = NULL;

        if (module_request.response) {
            KINFO("MODULE", "Bootloader provided %lu module(s)",
                  module_request.response->module_count);
            for (u64 i = 0; i < module_request.response->module_count; i++) {
                struct limine_file *mod = module_request.response->modules[i];
                KINFO("MODULE", "  [%lu] path='%s' addr=%p size=%lu",
                      (u64)i, mod->path ? mod->path : "(null)",
                      mod->address, mod->size);

                if (strstr(mod->path, "wallpaper.bin")) {
                    wallpaper_data = mod->address;
                    g_wallpaper_module = mod->address;
                    KINFO("MODULE", "    -> matched WALLPAPER");
                } else if (strstr(mod->path, "logo.bin")) {
                    logo_data = mod->address;
                    KINFO("MODULE", "    -> matched LOGO");
                } else if (strstr(mod->path, "authd.elf")) {
                    g_authd_module = mod->address;
                    g_authd_module_size = mod->size;
                    KINFO("MODULE", "    -> matched AUTHD APP @ %p", g_authd_module);
                } else if (strstr(mod->path, "hello.elf")) {
                    g_hello_module = mod->address;
                    g_hello_module_size = mod->size;
                    KINFO("MODULE", "    -> matched HELLO APP @ %p", g_hello_module);
                } else if (strstr(mod->path, "exec-test.elf")) {
                    g_exec_test_module = mod->address;
                    g_exec_test_module_size = mod->size;
                    KINFO("MODULE", "    -> matched EXEC TEST APP @ %p", g_exec_test_module);
                } else if (strstr(mod->path, "orphan-test.elf")) {
                    g_orphan_test_module = mod->address;
                    g_orphan_test_module_size = mod->size;
                    KINFO("MODULE", "    -> matched ORPHAN TEST APP @ %p", g_orphan_test_module);
                }
            }
        } else {
            KWARN("MODULE", "module_request.response is NULL — bootloader loaded no modules!");
        }

        KINFO("SPLASH", "wallpaper=%p  logo=%p", wallpaper_data, logo_data);

        if (wallpaper_data) {
            u32 *wp = (u32 *)wallpaper_data;
            KINFO("SPLASH", "wallpaper dimensions: %ux%u", wp[0], wp[1]);
        }
        if (logo_data) {
            u32 *lp = (u32 *)logo_data;
            KINFO("SPLASH", "logo dimensions: %ux%u", lp[0], lp[1]);
        }

        fb_enable_text(false);
        KINFO("SPLASH", "Drawing splash...");
        fb_draw_splash(wallpaper_data, logo_data);
        KINFO("SPLASH", "Splash drawn OK");
    } else {
        KWARN("SPLASH", "No framebuffer, skipping splash");
    }

    /* ---- Print boot banner (goes to serial since fb text is off) ---- */
    KINFO("BOOT", "--- Kernel Init Phases ---");
    print_banner();

    /* ---- Phase 4: CPU Setup ---- */
    KINFO("INIT", "Phase 1: CPU Setup");
    kprintf_color(0xFFFFDD00, "=== Phase 1: CPU Setup ===\n");
    gdt_init(0);
    KTRACE("INIT", "GDT OK");
    idt_init();
    KTRACE("INIT", "IDT OK");
    cpuid_init();
    KTRACE("INIT", "CPUID OK");
    tsc_init();
    KTRACE("INIT", "TSC OK");
    security_init();
    KTRACE("INIT", "Security OK");

    /* ---- Phase 5: Memory Management ---- */
    KINFO("INIT", "Phase 2: Memory Management");
    kprintf_color(0xFFFFDD00, "\n=== Phase 2: Memory (Cell Allocator) ===\n");
    
    if (!hhdm_request.response) {
        kpanic("HHDM request not fulfilled by bootloader");
    }
    u64 hhdm_offset = hhdm_request.response->offset;
    vmm_init(hhdm_offset);
    KTRACE("INIT", "VMM OK");

    if (!memmap_request.response) {
        kpanic("Memory map request not fulfilled by bootloader");
    }
    pmm_init((void *)memmap_request.response, hhdm_offset);
    KTRACE("INIT", "PMM OK");
    heap_init();
    KTRACE("INIT", "Heap OK");

    /* ---- Phase 5b: Modern interrupt + SMP topology ---- */
    KINFO("INIT", "Phase 2b: ACPI / APIC / SMP");
    kprintf_color(0xFFFFDD00, "\n=== Phase 2b: ACPI / APIC / SMP ===\n");
    acpi_init(rsdp_request.response ? rsdp_request.response->address : NULL,
              hhdm_offset);
    KTRACE("INIT", "ACPI OK");
    apic_init(hhdm_offset);
    KTRACE("INIT", "APIC OK");
    ioapic_init(hhdm_offset);
    KTRACE("INIT", "IOAPIC OK");
    hpet_init(hhdm_offset);
    KTRACE("INIT", "HPET OK");
    
    /* Route legacy ISA IRQs to BSP (LAPIC 0) */
    ioapic_set_irq(1, 33, 0);  /* Keyboard -> Vector 33 */
    ioapic_set_irq(12, 44, 0); /* PS/2 Mouse -> Vector 44 */
    
    percpu_init(0, 0);
    KTRACE("INIT", "PerCPU OK");
    smp_init(smp_request.response);
    KTRACE("INIT", "SMP OK");
    syscall_init();
    KTRACE("INIT", "Syscall OK");

    /* ---- Phase 6: YamGraph Core ---- */
    KINFO("INIT", "Phase 3: YamGraph Resource Graph");
    kprintf_color(0xFFFFDD00, "\n=== Phase 3: YamGraph Resource Graph ===\n");
    yamgraph_init();

    /* Register kernel subsystems as graph nodes */
    yam_node_id_t pmm_node = yamgraph_node_create(YAM_NODE_NAMESPACE, "pmm", NULL);
    yam_node_id_t vmm_node = yamgraph_node_create(YAM_NODE_NAMESPACE, "vmm", NULL);
    yam_node_id_t heap_node = yamgraph_node_create(YAM_NODE_NAMESPACE, "heap", NULL);

    /* Link kernel → subsystems */
    yamgraph_edge_link(0, pmm_node,  YAM_EDGE_OWNS, YAM_PERM_ALL);
    yamgraph_edge_link(0, vmm_node,  YAM_EDGE_OWNS, YAM_PERM_ALL);
    yamgraph_edge_link(0, heap_node, YAM_EDGE_OWNS, YAM_PERM_ALL);

    kprintf("[YAMGRAPH] Kernel subsystems registered: nodes=%u, edges=%u\n",
            yamgraph_node_count(), yamgraph_edge_count());

    /* ---- Phase 7: Self-Tests ---- */
    KINFO("INIT", "Phase 4: Self-Tests");
    kprintf_color(0xFFFFDD00, "\n=== Phase 4: Self-Tests ===\n");
    pmm_self_test();
    yamgraph_self_test();
    KINFO("INIT", "Self-tests passed");

    /* ---- Phase 8: System Ready ---- */
    kprintf_color(0xFF00FF88,
        "\n"
        "  ╔══════════════════════════════════════════════════╗\n"
        "  ║          YamOS v0.4.0 — BOOT COMPLETE           ║\n"
        "  ╚══════════════════════════════════════════════════╝\n"
        "\n");

    /* ---- Phase 9: Drivers, Subsystems & Input ---- */
    KINFO("INIT", "Phase 5: Drivers & Subsystems");
    pit_init(100);
    keyboard_init();
    app_registry_init();

    if (!g_yamboot_safe) {
        pci_init();
        driver_manager_init();
        driver_bind_pci_inventory();
        block_init();
        ahci_init_all();
        virtio_blk_init_all();
        block_dump();
        bcache_init();
        usb_init();
        i2c_init(); spi_init();
        vfs_init();
        ipc_init();
        net_init();
        audio_init();
        installer_init();
        evdev_init();
        mouse_init();
    }

    /* ---- Phase 10: Multitasking & Services ---- */
    KINFO("INIT", "Phase 6: Scheduler & Core Services");
    sched_init();
    sched_install_timer();
    cgroup_init();
    oom_init();
    power_init();
    ai_accel_init();

    if (YAM_PREEMPTIVE) {
        apic_timer_start(100);
        sched_enable();
    }

    /* ---- Splash Screen Spinner Animation ---- */
    __asm__ volatile ("sti");
    for (int i = 0; i < 30; i++) {
        fb_draw_spinner(i);
        for (int t = 0; t < 5; t++) __asm__ volatile ("hlt");
    }

    fb_clear(FB_COLOR_DARK_BG);
    fb_enable_text(true);

    /* ---- Phase 11: System Init (PID 1) ---- */
    KINFO("BOOT", "Spawning PID 1...");
    extern void init_task(void *);
    sched_spawn("init", init_task, NULL, 0);

    /* ---- Phase 12: Wayland Graphical Shell ---- */
    if (YAM_WAYLAND) {
        drm_init();
        wl_compositor_init();
        sched_spawn("wayland", wl_compositor_task, NULL, 0);
    }

    /* Task 0 becomes the Idle Task */
    for (;;) {
        sched_yield();
        __asm__ volatile("hlt");
    }
}
