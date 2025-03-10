#include "rwkv.h"
#include "ggml.h"
#include "ggml-alloc.h"

#include <string>
#include <vector>
#include <cstring>
#include <cinttypes>
#include <cmath>
#include <fstream>
#include <unordered_map>
#include <memory>
#include <utility>

#define _FILE_OFFSET_BITS 64
// Puts an optional break point, if debug is enabled.
#define RWKV_MAYBE_BREAK

#include <sys/stat.h>

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#    define stat _stat64
#    define fstat _fstat64
#    define ftell _ftelli64
#    define fseek _fseeki64
#    if !defined(NDEBUG)
#        include <intrin.h>
#        define RWKV_MAYBE_BREAK __debugbreak()
#    endif
#else
#    if !defined(__APPLE__)
#        define ftell ftello
#        define fseek fseeko
#    endif
#endif

static_assert(sizeof(stat::st_size) >= 8, "File offsets should be 64-bit or else rwkv.cpp will not be able to load model files over 2 GB");
static_assert(sizeof(decltype(ftell(NULL))) >= 8, "File offsets should be 64-bit or else rwkv.cpp will not be able to load model files over 2 GB");

#include "rwkv_error_handling.inc"

#include "rwkv_utilities.inc"

#include "rwkv_file_format.inc"

#include "rwkv_model_loading.inc"

#include "rwkv_operators.inc"

#include "rwkv_graph.inc"

// API function.
struct rwkv_context * rwkv_init_from_file(const char * file_path, const uint32_t n_threads) {
    global_last_error = RWKV_ERROR_NONE;

    std::unique_ptr<struct rwkv_context> ctx(new(std::nothrow) struct rwkv_context());
    RWKV_ASSERT_NULL_MSG(RWKV_ERROR_CTX | RWKV_ERROR_ALLOC, ctx, "Failed to allocate rwkv_context");

    ctx->model = new(std::nothrow) struct rwkv_model();
    ctx->model->reference_count++;
    RWKV_ENSURE_OR_NULL(rwkv_load_model_from_file(file_path, *ctx->model));

    ctx->n_threads = n_threads;

    RWKV_ENSURE_OR_NULL(rwkv_measure_and_build_serial_context(*ctx->model, ctx->serial_graph));

    return ctx.release();
}

// API function.
struct rwkv_context * rwkv_clone_context(struct rwkv_context * ctx, const uint32_t n_threads) {
    std::unique_ptr<struct rwkv_context> clone(new(std::nothrow) struct rwkv_context());
    RWKV_ASSERT_NULL_MSG(RWKV_ERROR_CTX | RWKV_ERROR_ALLOC, clone, "Failed to allocate rwkv_context");

    clone->model = ctx->model;
    clone->model->reference_count++;

    clone->n_threads = n_threads;

    RWKV_ENSURE_OR_NULL(rwkv_measure_and_build_serial_context(*clone->model, clone->serial_graph));

    clone->last_used_sequence_length = 0;

    clone->print_errors = ctx->print_errors;

    return clone.release();
}

#include "rwkv_gpu_offload.inc"

#include "rwkv_eval.inc"

// API function.
// Provided for backwards compatibility.
extern "C" RWKV_API uint32_t rwkv_get_state_buffer_element_count(const struct rwkv_context * ctx) {
    return rwkv_get_state_len(ctx);
}

// API function.
// Provided for backwards compatibility.
extern "C" RWKV_API uint32_t rwkv_get_logits_buffer_element_count(const struct rwkv_context * ctx) {
    return rwkv_get_logits_len(ctx);
}

// API function.
size_t rwkv_get_n_vocab(const struct rwkv_context * ctx) {
    return (size_t) ctx->model->header.n_vocab;
}

// API function.
size_t rwkv_get_n_embed(const struct rwkv_context * ctx) {
    return (size_t) ctx->model->header.n_embed;
}

// API function.
size_t rwkv_get_n_layer(const struct rwkv_context * ctx) {
    return (size_t) ctx->model->header.n_layer;
}

// API function.
size_t rwkv_get_state_len(const struct rwkv_context * ctx) {
    const struct rwkv_file_header & header = ctx->model->header;

    if (ctx->model->arch_version_major >= 5) {
        return (size_t) header.n_embed * (2 + ctx->model->head_size) * (size_t) header.n_layer;
    } else {
        return (size_t) header.n_embed * 5 * (size_t) header.n_layer;
    }
}

// API function.
size_t rwkv_get_logits_len(const struct rwkv_context * ctx) {
    return (size_t) ctx->model->header.n_vocab;
}

// API function.
void rwkv_free(struct rwkv_context * ctx) {
    if (ctx == NULL) {
        return;
    }

    if (--ctx->model->reference_count == 0) {
        ggml_free(ctx->model->ggml_ctx);

        delete ctx->model;
    }

    ggml_free(ctx->serial_graph.ggml_ctx);

    if (ctx->last_used_sequence_length > 0) {
        ggml_free(ctx->sequential_graph.ggml_ctx);
    }

    std::unique_ptr<struct rwkv_context> rwkv_ctx(ctx);
}

// API function.
void rwkv_set_print_errors(struct rwkv_context * ctx, const bool print_errors) {
    bool * ptr = ctx ? &ctx->print_errors : &global_print_errors;
    *ptr = print_errors;
}

// API function.
bool rwkv_get_print_errors(const struct rwkv_context * ctx) {
    return ctx ? ctx->print_errors : global_print_errors;
}

// API function.
enum rwkv_error_flags rwkv_get_last_error(struct rwkv_context * ctx) {
    enum rwkv_error_flags * ptr = ctx ? &ctx->last_error : &global_last_error;
    enum rwkv_error_flags value = *ptr;
    *ptr = RWKV_ERROR_NONE;
    return value;
}

#include "rwkv_quantize.inc"

// API function.
const char * rwkv_get_system_info_string(void) {
    static std::string s;

    if (s.empty()) {
        s  = "";
        s += "AVX="       + std::to_string(ggml_cpu_has_avx())       + " ";
        s += "AVX2="      + std::to_string(ggml_cpu_has_avx2())      + " ";
        s += "AVX512="    + std::to_string(ggml_cpu_has_avx512())    + " ";
        s += "FMA="       + std::to_string(ggml_cpu_has_fma())       + " ";
        s += "NEON="      + std::to_string(ggml_cpu_has_neon())      + " ";
        s += "ARM_FMA="   + std::to_string(ggml_cpu_has_arm_fma())   + " ";
        s += "F16C="      + std::to_string(ggml_cpu_has_f16c())      + " ";
        s += "FP16_VA="   + std::to_string(ggml_cpu_has_fp16_va())   + " ";
        s += "WASM_SIMD=" + std::to_string(ggml_cpu_has_wasm_simd()) + " ";
        s += "BLAS="      + std::to_string(ggml_cpu_has_blas())      + " ";
        s += "SSE3="      + std::to_string(ggml_cpu_has_sse3())      + " ";
        s += "VSX="       + std::to_string(ggml_cpu_has_vsx());
    }

    return s.c_str();
}
