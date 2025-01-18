
// SPDX-License-Identifier: MIT

#include "smp.h"

#include "arrays.h"
#include "assertions.h"
#include "cpu/mmu.h"
#include "interrupt.h"
#include "isr_ctx.h"
#include "mutex.h"
#include "port/dtb.h"

#include <limine.h>

#ifdef __riscv
#include "cpu/riscv_sbi.h"
#endif

#define REQ __attribute__((section(".requests")))



// SMP status.
typedef struct {
    // CPU is currently running.
    bool  is_up;
    // CPU has been taken over from Limine.
    bool  did_jump;
    // Temporary stack pointer.
    void *tmp_stack;
    // Point to jump to after starting the CPU.
    void (*entrypoint)();
    // CPU-local data.
    cpulocal_t cpulocal;
} smp_status_t;

// CPUID to/from SMP index map.
typedef struct {
    size_t cpuid;
    int    cpu;
} smp_map_t;

// SMP index sort.
static int smp_cpu_cmp(void const *a, void const *b) {
    smp_map_t const *map_a = a;
    smp_map_t const *map_b = b;
    if (map_a->cpu < map_b->cpu) {
        return -1;
    } else if (map_a->cpu > map_b->cpu) {
        return 1;
    } else {
        return 0;
    }
}

// SMP index sort.
static int smp_cpuid_cmp(void const *a, void const *b) {
    smp_map_t const *map_a = a;
    smp_map_t const *map_b = b;
    if (map_a->cpuid < map_b->cpuid) {
        return -1;
    } else if (map_a->cpuid > map_b->cpuid) {
        return 1;
    } else {
        return 0;
    }
}

// Number of detected CPU cores.
int smp_count = 1;

// CPUID to SMP index map length.
static size_t     smp_map_len;
// CPUID to SMP index map.
static smp_map_t *smp_map;
// SMP index to CPUID map length.
static size_t     smp_unmap_len;
// SMP index to CPUID map.
static smp_map_t *smp_unmap;

// Current status per CPU.
static smp_status_t *cpu_status;
#ifdef __riscv
// Whether the SBI supports HSM.
static bool sbi_supports_hsm;
#endif

#ifdef __riscv
#define smp_resp_procid hartid
#elif defined(__x86_64__)
#define smp_resp_procid lapic_id
#endif


static REQ struct limine_smp_request smp_req = {
    .id       = LIMINE_SMP_REQUEST,
    .revision = 2,
    .flags    = 0,
};


#ifdef PORT_ENABLE_DTB
// Initialise the SMP subsystem.
void smp_init_dtb(dtb_handle_t *dtb) {
#ifdef __riscv
    sbi_ret_t res = sbi_probe_extension(SBI_HART_MGMT_EID);
    sbi_supports_hsm == res.retval && !res.status;
    if (sbi_supports_hsm) {
        // SBI supports HSM; CPUs can be started and stopped.
        logk(LOG_DEBUG, "SBI supports HSM");
    } else {
        // SBI doesn't support HSM; CPUs can be started but not stopped.
        logk(LOG_DEBUG, "SBI doesn't support HSM");
    }
#endif

    // Parse CPU ID information from the DTB.
    dtb_node_t *cpus = dtb_get_node(dtb, dtb_root_node(dtb), "cpus");
    assert_always(cpus);
    dtb_node_t *cpu = cpus->nodes;
    assert_always(cpu);
    uint32_t cpu_acells = dtb_read_uint(dtb, cpus, "#address-cells");
    assert_always(cpu_acells && cpu_acells <= sizeof(size_t) / 4);
    assert_always(dtb_read_uint(dtb, cpus, "#size-cells") == 0);
#ifdef __riscv
    size_t bsp_cpuid = smp_req.response->bsp_hartid;
#elif defined(__x86_64__)
    size_t bsp_cpuid = smp_req.response->bsp_lapic_id;
#endif

    while (cpu) {
        // Detect usable architecture.
#ifdef __riscv
        uint32_t    isa_len = 0;
        char const *isa     = dtb_prop_content(dtb, dtb_get_prop(dtb, cpu, "riscv,isa"), &isa_len);
        if (isa_len < 5 || !cstr_prefix_equals(isa, __riscv_xlen == 32 ? "rv32i" : "rv64i", 5)) {
            cpu = cpu->next;
            continue;
        }
#elif defined(__x86_64__)
#else
#error "smp_init: Unsupported architecture"
#endif

        // Detect usable MMU.
        uint32_t    mmu_len = 0;
        char const *mmu     = dtb_prop_content(dtb, dtb_get_prop(dtb, cpu, "mmu-type"), &mmu_len);
        if (!mmu || !mmu_dtb_supported(mmu)) {
            cpu = cpu->next;
            continue;
        }

        // Read CPU ID.
        dtb_prop_t *reg = dtb_get_prop(dtb, cpu, "reg");
        assert_always(reg && reg->content_len == 4 * cpu_acells);
        size_t cpuid = dtb_prop_read_uint(dtb, reg);
        int    detected_cpu;
        if (cpuid == bsp_cpuid) {
            detected_cpu = 0;
        } else {
            detected_cpu = smp_count;
            smp_count++;
        }
        logkf(LOG_INFO, "Detected CPU #%{d} ID %{size;d}", detected_cpu, cpuid);

        // Add to the maps.
        smp_map_t new_ent = {
            .cpu   = detected_cpu,
            .cpuid = cpuid,
        };
        assert_always(array_len_sorted_insert(&smp_map, sizeof(smp_map_t), &smp_map_len, &new_ent, smp_cpuid_cmp));
        assert_always(array_len_sorted_insert(&smp_unmap, sizeof(smp_map_t), &smp_unmap_len, &new_ent, smp_cpu_cmp));

        cpu = cpu->next;
    }
    int cur_cpu = smp_cur_cpu();

    // Allocate status per CPU.
    cpu_status = calloc(smp_count, sizeof(smp_status_t));
    assert_always(cpu_status);
    cpu_status[cur_cpu] = (smp_status_t){
        .did_jump = true,
        .is_up    = true,
    };

    // Transfer booting CPU's CPU-local data to this array.
    bool ie = irq_disable();

    cpu_status[cur_cpu].cpulocal = *isr_ctx_get()->cpulocal;
    isr_ctx_get()->cpulocal      = &cpu_status[cur_cpu].cpulocal;

    irq_enable_if(ie);
}
#endif

// The the SMP CPU index of the calling CPU.
int smp_cur_cpu() {
    if (!smp_map_len) {
        return 0;
    }
    return isr_ctx_get()->cpulocal->cpu;
}

// Get the SMP CPU index from the CPU ID value.
int smp_get_cpu(size_t cpuid) {
    smp_map_t         dummy = {.cpuid = cpuid};
    array_binsearch_t res   = array_binsearch(smp_map, sizeof(smp_map_t), smp_map_len, &dummy, smp_cpuid_cmp);
    if (res.found) {
        return smp_map[res.index].cpu;
    }
    return -1;
}

// Get the CPU ID value from the SMP CPU index.
size_t smp_get_cpuid(int cpu) {
    smp_map_t         dummy = {.cpu = cpu};
    array_binsearch_t res   = array_binsearch(smp_unmap, sizeof(smp_map_t), smp_unmap_len, &dummy, smp_cpu_cmp);
    if (res.found) {
        return smp_map[res.index].cpuid;
    }
    return -1;
}



// First stage entrypoint for secondary CPUs.
static NAKED void cpu1_init0_limine(__attribute__((used)) struct limine_smp_info *info) {
    // clang-format off
#ifdef __riscv
    asm(
        ".option push;"
        ".option norelax;"
        "la gp, __global_pointer$;"
        ".option pop;"
        "j cpu1_init1_limine;"
    );
#elif defined(__x86_64__)
#endif
    // clang-format on
}

// Second stage entrypoint for secondary CPUs.
static void cpu1_init1_limine(struct limine_smp_info *info) {
    int       cur_cpu       = (int)info->extra_argument;
    isr_ctx_t tmp_ctx       = {0};
    tmp_ctx.flags           = ISR_CTX_FLAG_KERNEL;
    tmp_ctx.cpulocal        = &cpu_status[cur_cpu].cpulocal;
    tmp_ctx.cpulocal->cpuid = info->smp_resp_procid;
    tmp_ctx.cpulocal->cpu   = cur_cpu;
#ifdef __riscv
    asm("csrw sscratch, %0" ::"r"(&tmp_ctx));
#elif defined(__x86_64__)
#endif
    cpu_status[cur_cpu].entrypoint();
    __builtin_trap();
}

// Power on another CPU.
bool smp_poweron(int cpu, void *entrypoint, void *stack) {
    assert_dev_drop(cpu <= smp_count);
    if (cpu_status[cpu].is_up) {
        return false;
    }

    cpu_status[cpu].entrypoint = entrypoint;
    cpu_status[cpu].tmp_stack  = stack;

    // Start the CPU up.
    if (!cpu_status[cpu].did_jump) {
        for (uint64_t i = 0; i < smp_req.response->cpu_count; i++) {
            if (smp_req.response->cpus[i]->smp_resp_procid == smp_map[cpu].cpuid) {
                smp_req.response->cpus[i]->extra_argument = cpu;
                atomic_store(&smp_req.response->cpus[i]->goto_address, &cpu1_init0_limine);
                cpu_status[cpu].did_jump = true;
                return true;
            }
        }
        return false;
    } else {
        return false;
    }
}

// Power off this CPU.
bool smp_poweroff() {
    return false;
}

// Pause this CPU, if supported.
bool smp_pause() {
    return false;
}

// Resume another CPU, if supported.
bool smp_resume(int cpu) {
    return false;
}
