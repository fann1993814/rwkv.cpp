static void rwkv_validate_tensors_for_custom_unary_op(struct ggml_tensor * dest, const struct ggml_tensor * src) {
    GGML_ASSERT(dest->type == GGML_TYPE_F32);
    GGML_ASSERT(src->type == GGML_TYPE_F32);
    GGML_ASSERT(ggml_is_contiguous(dest));
    GGML_ASSERT(ggml_is_contiguous(src));
    GGML_ASSERT(ggml_are_same_shape(src, dest));
    // Verify that the shape is 2D.
    GGML_ASSERT(dest->ne[2] == 1);
    GGML_ASSERT(dest->ne[3] == 1);
}

#define SUPPRESS_UNUSED_WARNINGS_IN_CUSTOM_OP() { (void) ith; (void) nth; (void) userdata; }

static void rwkv_exp_impl(struct ggml_tensor * dest, const struct ggml_tensor * src, int ith, int nth, void * userdata) {
    rwkv_validate_tensors_for_custom_unary_op(dest, src);

    int64_t element_count = src->ne[0] * src->ne[1];
    float * src_data = (float *) src->data;
    float * dest_data = (float *) dest->data;

    for (int64_t i = 0; i < element_count; i++) {
        dest_data[i] = expf(src_data[i]);
    }

    SUPPRESS_UNUSED_WARNINGS_IN_CUSTOM_OP();
}

static void rwkv_1_minus_x_impl(struct ggml_tensor * dest, const struct ggml_tensor * src, int ith, int nth, void * userdata) {
    rwkv_validate_tensors_for_custom_unary_op(dest, src);

    int64_t element_count = src->ne[0] * src->ne[1];
    float * src_data = (float *) src->data;
    float * dest_data = (float *) dest->data;

    for (int64_t i = 0; i < element_count; i++) {
        dest_data[i] = 1.0F - src_data[i];
    }

    SUPPRESS_UNUSED_WARNINGS_IN_CUSTOM_OP();
}

static void rwkv_sigmoid_impl(struct ggml_tensor * dest, const struct ggml_tensor * src, int ith, int nth, void * userdata) {
    rwkv_validate_tensors_for_custom_unary_op(dest, src);

    int64_t element_count = src->ne[0] * src->ne[1];
    float * src_data = (float *) src->data;
    float * dest_data = (float *) dest->data;

    for (int64_t i = 0; i < element_count; i++) {
        dest_data[i] = 1.0F / (1.0F + expf(-src_data[i]));
    }

    SUPPRESS_UNUSED_WARNINGS_IN_CUSTOM_OP();
}

static void rwkv_max_impl(
    struct ggml_tensor * dest,
    const struct ggml_tensor * src0,
    const struct ggml_tensor * src1,
    int ith,
    int nth,
    void * userdata
) {
    GGML_ASSERT(dest->type == GGML_TYPE_F32);
    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT(src1->type == GGML_TYPE_F32);
    GGML_ASSERT(ggml_is_contiguous(dest));
    GGML_ASSERT(ggml_is_contiguous(src0));
    GGML_ASSERT(ggml_is_contiguous(src1));
    GGML_ASSERT(ggml_are_same_shape(src0, dest));
    GGML_ASSERT(ggml_are_same_shape(src1, dest));
    // Verify that the shape is 2D.
    GGML_ASSERT(dest->ne[2] == 1);
    GGML_ASSERT(dest->ne[3] == 1);

    int64_t element_count = src0->ne[0] * src0->ne[1];
    float * src0_data = (float *) src0->data;
    float * src1_data = (float *) src1->data;
    float * dest_data = (float *) dest->data;

    for (int64_t i = 0; i < element_count; i++) {
        dest_data[i] = fmaxf(src0_data[i], src1_data[i]);
    }

    SUPPRESS_UNUSED_WARNINGS_IN_CUSTOM_OP();
}

// Element-wise exp(x)
struct ggml_tensor * rwkv_exp(struct ggml_context * ctx, struct ggml_tensor * x) {
    return ggml_map_custom1(ctx, x, rwkv_exp_impl, 1, NULL);
}

// Element-wise 1 - x
struct ggml_tensor * rwkv_1_minus_x(struct ggml_context * ctx, struct ggml_tensor * x) {
    return ggml_map_custom1(ctx, x, rwkv_1_minus_x_impl, 1, NULL);
}

// Element-wise sigmoid(x)
struct ggml_tensor * rwkv_sigmoid_inplace(struct ggml_context * ctx, struct ggml_tensor * x) {
    return ggml_map_custom1_inplace(ctx, x, rwkv_sigmoid_impl, 1, NULL);
}

// Element-wise max(x, y)
struct ggml_tensor * rwkv_max(struct ggml_context * ctx, struct ggml_tensor * x, struct ggml_tensor * y) {
    return ggml_map_custom2(ctx, x, y, rwkv_max_impl, 1, NULL);
}

struct ggml_tensor * rwkv_layer_norm(struct ggml_context * ctx, struct ggml_tensor * x, struct ggml_tensor * weight, struct ggml_tensor * bias) {
    // LayerNorm in RWKV is `x = (x - mean(x)) / sqrt(variance(x) + 1e-5) * weight + bias`
    // Looks like ggml_norm does the first part, we only need to apply weight & bias.
    return ggml_add_inplace(ctx, ggml_mul_inplace(ctx, ggml_norm(ctx, x, 1e-5F), weight), bias);
}

static struct ggml_tensor * rwkv_transpose_then_cont(struct ggml_context * ctx, struct ggml_tensor * x) {
    return ggml_cont(ctx, ggml_transpose(ctx, x));
}

static struct ggml_tensor * rwkv_get_from_dim_3(struct ggml_context * ctx, struct ggml_tensor * x, int64_t index) {
    return ggml_view_4d(
        ctx,
        x,
        x->ne[0],
        x->ne[1],
        x->ne[2],
        1,
        x->nb[1],
        x->nb[2],
        x->nb[3],
        index * (x->ne[0] * x->ne[1] * x->ne[2]) * sizeof(float)
    );
}

static struct ggml_tensor * rwkv_flatten(struct ggml_context * ctx, struct ggml_tensor * x) {
    return ggml_view_1d(ctx, x, ggml_nelements(x), 0);
}
