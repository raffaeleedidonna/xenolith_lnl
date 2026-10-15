/* Exercise the real prefill dispatch with a recording Level Zero driver. */
#define zeKernelCreate test_zeKernelCreate
#define zeKernelSetGroupSize test_zeKernelSetGroupSize
#define zeKernelSetArgumentValue test_zeKernelSetArgumentValue
#define zeCommandListAppendLaunchKernel test_zeCommandListAppendLaunchKernel
#define zeCommandListHostSynchronize test_zeCommandListHostSynchronize
#include "../xenolith.c"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

typedef struct {
    uint64_t arguments[16];
} test_kernel;

static test_kernel causal_kernel, visual_kernel, global_kernel;
static ze_kernel_handle_t last_kernel;
static unsigned creates, group_sizes, launches;

ze_result_t ZE_APICALL test_zeKernelCreate(ze_module_handle_t module,
        const ze_kernel_desc_t *desc, ze_kernel_handle_t *out) {
    assert(module);
    assert(desc->stype == ZE_STRUCTURE_TYPE_KERNEL_DESC);
    assert(!strcmp(desc->pKernelName, "xe_prefill_attn_online_b8_swa_visual"));
    *out = (ze_kernel_handle_t)&visual_kernel;
    creates++;
    return ZE_RESULT_SUCCESS;
}

ze_result_t ZE_APICALL test_zeKernelSetGroupSize(ze_kernel_handle_t kernel,
        uint32_t x, uint32_t y, uint32_t z) {
    assert(kernel == (ze_kernel_handle_t)&visual_kernel);
    assert(x == 128 && y == 1 && z == 1);
    group_sizes++;
    return ZE_RESULT_SUCCESS;
}

ze_result_t ZE_APICALL test_zeKernelSetArgumentValue(ze_kernel_handle_t kernel,
        uint32_t index, size_t size, const void *value) {
    test_kernel *record = (test_kernel *)kernel;
    assert(record && index < 16 && size <= sizeof record->arguments[index]);
    record->arguments[index] = 0;
    memcpy(&record->arguments[index], value, size);
    return ZE_RESULT_SUCCESS;
}

ze_result_t ZE_APICALL test_zeCommandListAppendLaunchKernel(
        ze_command_list_handle_t commands, ze_kernel_handle_t kernel,
        const ze_group_count_t *groups, ze_event_handle_t event,
        uint32_t wait_count, ze_event_handle_t *waits) {
    assert(commands && kernel && groups->groupCountX > 0);
    assert(groups->groupCountY == 1 && groups->groupCountZ == 1);
    assert(!event && !wait_count && !waits);
    last_kernel = kernel;
    launches++;
    return ZE_RESULT_SUCCESS;
}

ze_result_t ZE_APICALL test_zeCommandListHostSynchronize(
        ze_command_list_handle_t commands, uint64_t timeout) {
    assert(commands && timeout == UINT64_MAX);
    return ZE_RESULT_SUCCESS;
}

static void attention_case(xe_engine *e, xe_attention_mode mode,
                           int dimension, test_kernel *expected) {
    float q = 0, output = 0;
    _Float16 k = 0, v = 0;
    int heads = dimension == 256 ? 8 : 2;
    unsigned before = launches;
    xe_prefill_attention_online_append(e, &q, &k, &v, &output,
                                        17, 24, dimension, heads, 7, 1024, mode);
    assert(launches == before + 1 && last_kernel == (ze_kernel_handle_t)expected);
    assert(expected->arguments[0] == (uintptr_t)&q);
    assert(expected->arguments[1] == (uintptr_t)&k);
    assert(expected->arguments[2] == (uintptr_t)&v);
    assert(expected->arguments[3] == (uintptr_t)&output);
    assert(expected->arguments[4] == 17 && expected->arguments[5] == 24);
    assert(expected->arguments[8] == (unsigned)heads && expected->arguments[9] == 7);
    if (dimension == 256) {
        assert(expected->arguments[6] == 256 && expected->arguments[10] == 1024);
    } else {
        assert(expected->arguments[6] == 17 && expected->arguments[10] == 0);
    }
}

int main(void) {
    xe_engine e = {0};
    e.gpu.module = (ze_module_handle_t)(uintptr_t)1;
    e.gpu.commands = (ze_command_list_handle_t)(uintptr_t)1;
    e.gpu.prefill_attn_online_b8_swa = (ze_kernel_handle_t)&causal_kernel;
    e.gpu.prefill_attn_online_b8_global_shared = (ze_kernel_handle_t)&global_kernel;
    attention_case(&e, XE_ATTENTION_VISUAL, 512, &global_kernel);
    attention_case(&e, XE_ATTENTION_CAUSAL, 256, &causal_kernel);
    assert(creates == 0 && group_sizes == 0);
    attention_case(&e, XE_ATTENTION_VISUAL, 256, &visual_kernel);
    assert(creates == 1 && group_sizes == 1);
    attention_case(&e, XE_ATTENTION_CAUSAL, 256, &causal_kernel);
    attention_case(&e, XE_ATTENTION_VISUAL, 256, &visual_kernel);
    attention_case(&e, XE_ATTENTION_CAUSAL, 512, &global_kernel);
    assert(creates == 1 && group_sizes == 1);
    assert(e.gpu.prefill_attn_online_b8_swa_visual == (ze_kernel_handle_t)&visual_kernel);
    for (size_t i = 0; i < sizeof e.gpu.vision / sizeof *e.gpu.vision; i++)
        assert(!e.gpu.vision[i]);
    puts("Prefill attention dispatch: PASS");
    return 0;
}
