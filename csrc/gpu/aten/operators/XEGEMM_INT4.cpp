#if defined(USE_XETLA)
#include "XEGEMM_INT4.h"
#include <ATen/ATen.h>
#include <ATen/CPUApplyUtils.h>
#include <ATen/record_function.h>
#include <runtime/Utils.h>
#include "comm/ATDispatch.h"
#ifndef USE_PRIMITIVE_CACHE
#include "oneDNN/WoqMatmul.h"
#else
#include "oneDNN/DnnlMatmulQuant.h"
#endif
#include "utils/CustomOperatorRegistration.h"

namespace at {
namespace AtenIpexTypeXPU {

bool choose_recommand_compute_eng() {
  auto compute_eng = Settings::I().get_compute_eng();
  DeviceId curDevID = at::xpu::current_device();
  bool is_2d_block = dpcppGetDeviceHas2DBlock(curDevID);
  bool is_xmx = dpcppGetDeviceHasXMX(curDevID);

  return (is_2d_block && is_xmx)
      ? (compute_eng == torch_ipex::xpu::COMPUTE_ENG::XETLA)
      : (compute_eng == torch_ipex::xpu::COMPUTE_ENG::RECOMMEND ||
         compute_eng == torch_ipex::xpu::COMPUTE_ENG::XETLA);
}

static void mm_qkv_out_wint4(
    const Tensor& input_,
    const Tensor& weight_,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    const optional<Tensor>& bias_,
    Tensor& out0_,
    Tensor& out1_,
    Tensor& out2_,
    int64_t group_size) {
  if (choose_recommand_compute_eng()) {
    auto input = input_.flatten(0, -2);
    if (input.scalar_type() == ScalarType::Float)
      input = input.to(at::kHalf);
    auto weight = weight_.flatten(0, -2);
    auto out0 = out0_.flatten(0, -2);
    auto out1 = out1_.flatten(0, -2);
    auto out2 = out2_.flatten(0, -2);
    // input: m,k; weight: (n_q+n_k+n_v),k bias(opt): (n_q+n_k+n_v)
    TORCH_CHECK(input.dim() == 2);
    TORCH_CHECK(out0.dim() == 2 && out1.dim() == 2 && out2.dim() == 2);
    int m = input.sizes()[0];
    int k = input.sizes()[1];
    int n = weight.sizes()[0];

    bool has_bias = bias_.has_value();
    if (has_bias) {
      auto bias = bias_.value();
      TORCH_CHECK(bias.dim() == 1 && bias.sizes()[0] == n);
    }
    TORCH_CHECK(
        out0.sizes()[0] == m && out1.sizes()[0] == m && out2.sizes()[0] == m);
    TORCH_CHECK(n == (out0.sizes()[1] + out1.sizes()[1] + out2.sizes()[1]));
    TORCH_CHECK(input.is_contiguous() && weight.is_contiguous());
    TORCH_CHECK(
        input.scalar_type() == kHalf || input.scalar_type() == kBFloat16);
    TORCH_CHECK(
        weight.scalar_type() == kQUInt8 || weight.scalar_type() == kByte ||
        weight.scalar_type() == kChar || weight.scalar_type() == kInt)

    gpu::xetla::gpu_arch arch_tag = gpu::xetla::get_xetla_current_arch_tag();
    auto policy = HGEMMXetla_INT4()
                      .add_matrix_out(out0)
                      .add_matrix_out(out1)
                      .add_matrix_out(out2)
                      .add_matrix_inp(input)
                      .add_matrix_wei(weight)
                      .add_matrix_scl(weight_scl)
                      .add_matrix_zp(weight_zp);
    if (has_bias)
      policy.add_epilogue(bias_.value(), HGEMMXetla_INT4::EpilogueType::BIAS);
    policy.add_epilogue(Tensor(), HGEMMXetla_INT4::EpilogueType::SPLIT3)
        .add_group_size(group_size)
        .add_arch(arch_tag)
        .build();
    TORCH_CHECK(
        policy.fallback() == false,
        has_bias ? "qkv bias: " : "qkv: ",
        "invalid gemm shape");
    policy.run();
  } else {
    Attr attr;
    if (bias_.has_value()) {
      auto bias = bias_.value();
#ifndef USE_PRIMITIVE_CACHE
      out0_ = torch_ipex::xpu::oneDNN::woq_matmul_int4(
          out0_,
          input_,
          weight_[0],
          weight_scl[0],
          weight_zp[0],
          group_size,
          false,
          attr,
          std::nullopt,
          bias[0]);
      out1_ = torch_ipex::xpu::oneDNN::woq_matmul_int4(
          out1_,
          input_,
          weight_[1],
          weight_scl[1],
          weight_zp[1],
          group_size,
          false,
          attr,
          std::nullopt,
          bias[1]);
      out2_ = torch_ipex::xpu::oneDNN::woq_matmul_int4(
          out2_,
          input_,
          weight_[2],
          weight_scl[2],
          weight_zp[2],
          group_size,
          false,
          attr,
          std::nullopt,
          bias[2]);
#else // USE_PRIMITIVE_CACHE
      out0_ = torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16(
          out0_,
          input_,
          weight_[0],
          bias[0],
          weight_scl[0],
          weight_zp[0],
          group_size,
          false,
          std::nullopt);
      out1_ = torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16(
          out1_,
          input_,
          weight_[1],
          bias[1],
          weight_scl[1],
          weight_zp[1],
          group_size,
          false,
          std::nullopt);
      out2_ = torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16(
          out2_,
          input_,
          weight_[2],
          bias[2],
          weight_scl[2],
          weight_zp[2],
          group_size,
          false,
          std::nullopt);
#endif // USE_PRIMITIVE_CACHE
    } else {
#ifndef USE_PRIMITIVE_CACHE
      out0_ = torch_ipex::xpu::oneDNN::woq_matmul_int4(
          out0_,
          input_,
          weight_[0],
          weight_scl[0],
          weight_zp[0],
          group_size,
          false,
          attr,
          std::nullopt);
      out1_ = torch_ipex::xpu::oneDNN::woq_matmul_int4(
          out1_,
          input_,
          weight_[1],
          weight_scl[1],
          weight_zp[1],
          group_size,
          false,
          attr,
          std::nullopt);
      out2_ = torch_ipex::xpu::oneDNN::woq_matmul_int4(
          out2_,
          input_,
          weight_[2],
          weight_scl[2],
          weight_zp[2],
          group_size,
          false,
          attr,
          std::nullopt);
#else // USE_PRIMITIVE_CACHE
      out0_ = torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16(
          out0_,
          input_,
          weight_[0],
          std::nullopt,
          weight_scl[0],
          weight_zp[0],
          group_size,
          false,
          std::nullopt);
      out1_ = torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16(
          out1_,
          input_,
          weight_[1],
          std::nullopt,
          weight_scl[1],
          weight_zp[1],
          group_size,
          false,
          std::nullopt);
      out2_ = torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16(
          out2_,
          input_,
          weight_[2],
          std::nullopt,
          weight_scl[2],
          weight_zp[2],
          group_size,
          false,
          std::nullopt);
#endif // USE_PRIMITIVE_CACHE
    }
  }
}

static std::tuple<Tensor, Tensor, Tensor> mm_qkv_wint4(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    const optional<Tensor>& bias_,
    int64_t n_key,
    int64_t n_value,
    int64_t group_size) {
  auto input_flat = input.flatten(0, -2);
  if (input_flat.scalar_type() == ScalarType::Float)
    input_flat = input_flat.to(at::kHalf);
  auto weight_flat = weight.flatten(0, -2);
  int m = input_flat.sizes()[0];
  int k = input_flat.sizes()[1];
  int n = weight_flat.sizes()[0];
  const auto n_query = n - n_key - n_value;
  auto out0 = at::empty({m, n_query}, input.options());
  auto out1 = at::empty({m, n_key}, input.options());
  auto out2 = at::empty({m, n_value}, input.options());
  mm_qkv_out_wint4(
      input,
      weight,
      weight_scl,
      weight_zp,
      bias_,
      out0,
      out1,
      out2,
      group_size);
  if (choose_recommand_compute_eng()) {
    const auto sizes = input.sym_sizes().vec();
    auto sizes0 = sizes, sizes1 = sizes, sizes2 = sizes;
    sizes0[sizes0.size() - 1] = n_query;
    sizes1[sizes1.size() - 1] = n_key;
    sizes2[sizes2.size() - 1] = n_value;
    return std::forward_as_tuple(
        out0.view_symint(sizes0),
        out1.view_symint(sizes1),
        out2.view_symint(sizes2));
  } else {
    return std::forward_as_tuple(out0, out1, out2);
  }
}

// mlp operators naming convention:
// mlp_[<gate_postop>]..._<binary_op>_[<up_postop>]...[_out]_int4
static inline HGEMMXetla_INT4 mlp_mul_dispatch(
    const Tensor& input_,
    const Tensor& gate_up_wei,
    const Tensor& gate_up_wei_scl,
    const Tensor& gate_up_wei_zp,
    int64_t group_size,
    const std::vector<std::tuple<const Tensor&, HGEMMXetla_INT4::EpilogueType>>&
        gate_post_ops,
    const std::vector<std::tuple<const Tensor&, HGEMMXetla_INT4::EpilogueType>>&
        up_post_ops,
    Tensor* const output) {
  auto input = input_.flatten(0, -2);
  if (input.scalar_type() == ScalarType::Float)
    input = input.to(at::kHalf);
  // input: m,k; gate_up_wei: 2,n,k; gate_proj: n,k
  int m = input.sizes()[0];
  int k = input.sizes()[1];
  int n = gate_up_wei.sizes()[1];
  *output = output->defined() ? output->flatten(0, -2)
                              : at::empty({m, n}, input.options());
  TORCH_CHECK(input.is_contiguous() && gate_up_wei.is_contiguous());
  TORCH_CHECK(input.scalar_type() == kHalf || input.scalar_type() == kBFloat16);
  TORCH_CHECK(
      gate_up_wei.scalar_type() == kChar ||
      gate_up_wei.scalar_type() == kByte ||
      gate_up_wei.scalar_type() == kQUInt8 ||
      gate_up_wei.scalar_type() == kInt);

  gpu::xetla::gpu_arch arch_tag = gpu::xetla::get_xetla_current_arch_tag();
  auto dispatcher = HGEMMXetla_INT4()
                        .add_matrix_out(*output)
                        .add_matrix_inp(input)
                        .add_matrix_wei(gate_up_wei)
                        .add_matrix_scl(gate_up_wei_scl)
                        .add_matrix_zp(gate_up_wei_zp);
  for (auto& [epilogue_, epilogue_type] : gate_post_ops) {
    dispatcher.add_epilogue(epilogue_, epilogue_type);
  }
  dispatcher.add_epilogue(Tensor(), HGEMMXetla_INT4::EpilogueType::GATE_UP_MUL);
  for (auto& [epilogue_, epilogue_type] : up_post_ops) {
    dispatcher.add_epilogue(epilogue_, epilogue_type);
  }
  dispatcher //
      .add_group_size(group_size)
      .add_arch(arch_tag)
      .build();
  TORCH_CHECK(dispatcher.fallback() == false, "mlp_mul: invalid gemm config");
  dispatcher.run();
  return dispatcher;
}
// silu(linear(input, gate_wei)) * linear(input, up_wei)
static Tensor mlp_silu_mul_int4(
    const Tensor& input,
    const Tensor& gate_up_wei,
    const Tensor& gate_up_wei_scl,
    const Tensor& gate_up_wei_zp,
    int64_t group_size) {
  Tensor out;
  mlp_mul_dispatch(
      input,
      gate_up_wei,
      gate_up_wei_scl,
      gate_up_wei_zp,
      group_size,
      {{{}, HGEMMXetla_INT4::EpilogueType::SILU}},
      {},
      &out);
  return resize_as_mat1(input, out);
}
// silu(linear(input, gate_wei)) * linear(input, up_wei)
static void mlp_silu_mul_out_int4(
    const Tensor& input,
    const Tensor& gate_up_wei,
    const Tensor& gate_up_wei_scl,
    const Tensor& gate_up_wei_zp,
    Tensor& out,
    int64_t group_size) {
  mlp_mul_dispatch(
      input,
      gate_up_wei,
      gate_up_wei_scl,
      gate_up_wei_zp,
      group_size,
      {{{}, HGEMMXetla_INT4::EpilogueType::SILU}},
      {},
      &out);
  return;
}
// silu(linear(input, gate_wei) + gate_bias) * linear(input, up_wei)
static Tensor mlp_bias_silu_mul_int4(
    const Tensor& input,
    const Tensor& gate_up_wei,
    const Tensor& gate_up_wei_scl,
    const Tensor& gate_up_wei_zp,
    const Tensor& gate_bias,
    int64_t group_size) {
  Tensor out;
  mlp_mul_dispatch(
      input,
      gate_up_wei,
      gate_up_wei_scl,
      gate_up_wei_zp,
      group_size,
      {{gate_bias.flatten(), HGEMMXetla_INT4::EpilogueType::BIAS},
       {{}, HGEMMXetla_INT4::EpilogueType::SILU}},
      {},
      &out);
  return resize_as_mat1(input, out);
}
// silu(linear(input, gate_wei) + gate_bias) * linear(input, up_wei)
static void mlp_bias_silu_mul_out_int4(
    const Tensor& input,
    const Tensor& gate_up_wei,
    const Tensor& gate_up_wei_scl,
    const Tensor& gate_up_wei_zp,
    Tensor& out,
    const Tensor& gate_bias,
    int64_t group_size) {
  mlp_mul_dispatch(
      input,
      gate_up_wei,
      gate_up_wei_scl,
      gate_up_wei_zp,
      group_size,
      {{gate_bias.flatten(), HGEMMXetla_INT4::EpilogueType::BIAS},
       {{}, HGEMMXetla_INT4::EpilogueType::SILU}},
      {},
      &out);
  return;
}
// silu(linear(input, gate_wei)) * (linear(input, up_wei) + up_bias)
static Tensor mlp_silu_mul_bias_int4(
    const Tensor& input,
    const Tensor& gate_up_wei,
    const Tensor& gate_up_wei_scl,
    const Tensor& gate_up_wei_zp,
    const Tensor& up_bias,
    int64_t group_size) {
  Tensor out;
  mlp_mul_dispatch(
      input,
      gate_up_wei,
      gate_up_wei_scl,
      gate_up_wei_zp,
      group_size,
      {{{}, HGEMMXetla_INT4::EpilogueType::SILU}},
      {{up_bias.flatten(), HGEMMXetla_INT4::EpilogueType::BIAS}},
      &out);
  return resize_as_mat1(input, out);
}
// silu(linear(input, gate_wei)) * (linear(input, up_wei) + up_bias)
static void mlp_silu_mul_bias_out_int4(
    const Tensor& input,
    const Tensor& gate_up_wei,
    const Tensor& gate_up_wei_scl,
    const Tensor& gate_up_wei_zp,
    Tensor& out,
    const Tensor& up_bias,
    int64_t group_size) {
  mlp_mul_dispatch(
      input,
      gate_up_wei,
      gate_up_wei_scl,
      gate_up_wei_zp,
      group_size,
      {{{}, HGEMMXetla_INT4::EpilogueType::SILU}},
      {{up_bias.flatten(), HGEMMXetla_INT4::EpilogueType::BIAS}},
      &out);
  return;
}
// silu(linear(input, gate_wei) + gate_bias) * (linear(input, up_wei) + up_bias)
static Tensor mlp_bias_silu_mul_bias_int4(
    const Tensor& input,
    const Tensor& gate_up_wei,
    const Tensor& gate_up_wei_scl,
    const Tensor& gate_up_wei_zp,
    const Tensor& gate_bias,
    const Tensor& up_bias,
    int64_t group_size) {
  Tensor out;
  mlp_mul_dispatch(
      input,
      gate_up_wei,
      gate_up_wei_scl,
      gate_up_wei_zp,
      group_size,
      {{gate_bias.flatten(), HGEMMXetla_INT4::EpilogueType::BIAS},
       {{}, HGEMMXetla_INT4::EpilogueType::SILU}},
      {{up_bias.flatten(), HGEMMXetla_INT4::EpilogueType::BIAS}},
      &out);
  return resize_as_mat1(input, out);
}
// silu(linear(input, gate_wei) + gate_bias) * (linear(input, up_wei) + up_bias)
static void mlp_bias_silu_mul_bias_out_int4(
    const Tensor& input,
    const Tensor& gate_up_wei,
    const Tensor& gate_up_wei_scl,
    const Tensor& gate_up_wei_zp,
    const Tensor& up_bias,
    Tensor& out,
    const Tensor& gate_bias,
    int64_t group_size) {
  mlp_mul_dispatch(
      input,
      gate_up_wei,
      gate_up_wei_scl,
      gate_up_wei_zp,
      group_size,
      {{gate_bias.flatten(), HGEMMXetla_INT4::EpilogueType::BIAS},
       {{}, HGEMMXetla_INT4::EpilogueType::SILU}},
      {{up_bias.flatten(), HGEMMXetla_INT4::EpilogueType::BIAS}},
      &out);
  return;
}

static inline HGEMMXetla_INT4 mm_int4_dispatch(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t group_size,
    const std::vector<std::tuple<const Tensor&, HGEMMXetla_INT4::EpilogueType>>&
        epilogues,
    Tensor* const output,
    const c10::optional<Tensor>& g_idx) {
  Tensor input_flat;
  if (g_idx.has_value()) {
    input_flat = input.index_select(-1, g_idx.value()).flatten(0, -2);
  } else {
    input_flat = input.flatten(0, -2);
  }
  auto weight_flat = weight.flatten(0, -2);
  if (input_flat.scalar_type() == ScalarType::Float)
    input_flat = input_flat.to(at::kHalf);
  int m = input_flat.sizes()[0];
  int k = input_flat.sizes()[1];
  int n = weight_flat.sizes()[0];
  *output = output->defined() ? output->flatten(0, -2)
                              : at::empty({m, n}, input.options());
  gpu::xetla::gpu_arch arch_tag = gpu::xetla::get_xetla_current_arch_tag();
  auto dispatcher = HGEMMXetla_INT4()
                        .add_matrix_out(*output)
                        .add_matrix_inp(input_flat)
                        .add_matrix_wei(weight_flat)
                        .add_matrix_scl(weight_scl)
                        .add_matrix_zp(weight_zp)
                        .add_group_size(group_size)
                        .add_arch(arch_tag);
  for (auto& [epilogue_, epilogue_type] : epilogues) {
    dispatcher.add_epilogue(epilogue_, epilogue_type);
  }
  dispatcher.build();
  TORCH_CHECK(dispatcher.fallback() == false, "mm int4: invalid gemm shape");
  dispatcher.run();
  return dispatcher;
}

static Tensor mm_bias_int4(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& bias_,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t group_size,
    const c10::optional<Tensor>& g_idx) {
  Tensor out;
  if (choose_recommand_compute_eng()) {
    auto bias = bias_.flatten();
    auto dispatcher = mm_int4_dispatch(
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        {{bias, HGEMMXetla_INT4::EpilogueType::BIAS}},
        &out,
        g_idx);
    return resize_as_mat1(input, out);
  } else {
#ifndef USE_PRIMITIVE_CACHE
    Attr attr;
    torch_ipex::xpu::oneDNN::woq_matmul_int4(
        out,
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        false,
        attr,
        g_idx,
        bias_);
#else // USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16(
        out,
        input,
        weight,
        bias_,
        weight_scl,
        weight_zp,
        group_size,
        false,
        g_idx);
#endif // USE_PRIMITIVE_CACHE
    return out;
  }
}

static Tensor mm_int4(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t group_size,
    const c10::optional<Tensor>& g_idx) {
  Tensor out;
  if (choose_recommand_compute_eng()) {
    auto dispatcher = mm_int4_dispatch(
        input, weight, weight_scl, weight_zp, group_size, {}, &out, g_idx);
    return resize_as_mat1(input, out);
  } else {
#ifndef USE_PRIMITIVE_CACHE
    at::Tensor bias = Tensor();
    Attr attr;
    torch_ipex::xpu::oneDNN::woq_matmul_int4(
        out,
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        false,
        attr,
        g_idx,
        bias);
#else // USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16(
        out,
        input,
        weight,
        std::nullopt,
        weight_scl,
        weight_zp,
        group_size,
        false,
        g_idx);
#endif // USE_PRIMITIVE_CACHE
    return out;
  }
}

static void mm_int4_out(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    Tensor& out,
    int64_t group_size,
    const c10::optional<Tensor>& g_idx) {
  if (choose_recommand_compute_eng()) {
    auto dispatcher = mm_int4_dispatch(
        input, weight, weight_scl, weight_zp, group_size, {}, &out, g_idx);
    return;
  } else {
    Attr attr;
#ifndef USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::woq_matmul_int4(
        out,
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        false,
        attr,
        g_idx);
#else // USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16(
        out,
        input,
        weight,
        std::nullopt,
        weight_scl,
        weight_zp,
        group_size,
        false,
        g_idx);
#endif // USE_PRIMITIVE_CACHE
    return;
  }
}

static Tensor mm_silu_int4(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t group_size,
    const c10::optional<Tensor>& g_idx) {
  Tensor out;
  if (choose_recommand_compute_eng()) {
    auto dispatcher = mm_int4_dispatch(
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        {{Tensor(), HGEMMXetla_INT4::EpilogueType::SILU}},
        &out,
        g_idx);
    return resize_as_mat1(input, out);
  } else {
#ifndef USE_PRIMITIVE_CACHE
    at::Tensor bias = Tensor();
    Attr attr;
    torch_ipex::xpu::oneDNN::woq_matmul_silu(
        out,
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        false,
        attr,
        g_idx,
        bias);
#else // USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_silu(
        out,
        input,
        weight,
        std::nullopt,
        weight_scl,
        weight_zp,
        group_size,
        false,
        g_idx);
#endif // USE_PRIMITIVE_CACHE
    return out;
  }
}

static Tensor mm_resmul_int4(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    const Tensor& res,
    int64_t group_size,
    const c10::optional<Tensor>& g_idx) {
  auto res_flat = res.flatten(0, -2);
  Tensor out;
  if (choose_recommand_compute_eng()) {
    auto dispatcher = mm_int4_dispatch(
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        {{res_flat, HGEMMXetla_INT4::EpilogueType::RES_MUL}},
        &out,
        g_idx);
    return resize_as_mat1(input, out);
  } else {
    Attr attr;
#ifndef USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::woq_matmul_resmul(
        out,
        input,
        weight,
        weight_scl,
        weight_zp,
        res,
        group_size,
        false,
        attr,
        g_idx);
#else // USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_resmul(
        out,
        input,
        weight,
        std::nullopt,
        weight_scl,
        weight_zp,
        res,
        group_size,
        false,
        g_idx);
#endif // USE_PRIMITIVE_CACHE
    return out;
  }
}

static Tensor mm_bias_gelu_int4(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    const Tensor& bias,
    int64_t group_size,
    c10::string_view approximate,
    const c10::optional<Tensor>& g_idx) {
  Tensor out;
  if (choose_recommand_compute_eng()) {
    auto bias_flat = bias.flatten();
    TORCH_CHECK(approximate == "tanh");
    auto dispatcher = mm_int4_dispatch(
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        {{bias_flat, HGEMMXetla_INT4::EpilogueType::BIAS},
         {Tensor(), HGEMMXetla_INT4::EpilogueType::GELU}},
        &out,
        g_idx);
    return resize_as_mat1(input, out);
  } else {
#ifndef USE_PRIMITIVE_CACHE
    Attr attr;
    torch_ipex::xpu::oneDNN::woq_matmul_bias_gelu(
        out,
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        approximate,
        false,
        attr,
        g_idx,
        bias);
#else // USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_bias_gelu(
        out,
        input,
        weight,
        bias,
        weight_scl,
        weight_zp,
        group_size,
        approximate,
        false,
        g_idx);
#endif // USE_PRIMITIVE_CACHE
    return out;
  }
}

static Tensor mm_bias_resadd_resadd_int4(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& bias,
    const Tensor& res0,
    const Tensor& res1,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t group_size,
    const c10::optional<Tensor>& g_idx) {
  Tensor out;
  if (choose_recommand_compute_eng()) {
    auto bias_flat = bias.flatten();
    auto res0_flat = res0.flatten(0, -2);
    auto res1_flat = res1.flatten(0, -2);
    auto dispatcher = mm_int4_dispatch(
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        {{bias_flat, HGEMMXetla_INT4::EpilogueType::BIAS},
         {res0_flat, HGEMMXetla_INT4::EpilogueType::RES_ADD},
         {res1_flat, HGEMMXetla_INT4::EpilogueType::RES_ADD}},
        &out,
        g_idx);
    return resize_as_mat1(input, out);
  } else {
#ifndef USE_PRIMITIVE_CACHE
    Attr attr;
    torch_ipex::xpu::oneDNN::woq_matmul_bias_resadd_resadd(
        out,
        input,
        weight,
        weight_scl,
        weight_zp,
        res0,
        res1,
        group_size,
        false,
        attr,
        g_idx,
        bias);
#else // USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_bias_resadd_resadd(
        out,
        input,
        weight,
        bias,
        weight_scl,
        weight_zp,
        res0,
        res1,
        group_size,
        false,
        g_idx);
#endif // USE_PRIMITIVE_CACHE
    return out;
  }
}

static Tensor mm_low_bits(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    const Tensor& bias,
    bool has_bias,
    const std::string& compute_dtype,
    const std::string& weight_dtype,
    int64_t group_size,
    const c10::optional<Tensor>& g_idx) {
  return has_bias
      ? mm_bias_int4(
            input, weight, bias, weight_scl, weight_zp, group_size, g_idx)
      : mm_int4(input, weight, weight_scl, weight_zp, group_size, g_idx);
}

static Tensor mm_silu_mul_int4(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t group_size,
    const Tensor& res,
    const c10::optional<Tensor>& g_idx) {
  Tensor out;
  if (choose_recommand_compute_eng()) {
    auto res_flat = res.flatten(0, -2);
    auto dispatcher = mm_int4_dispatch(
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        {{Tensor(), HGEMMXetla_INT4::EpilogueType::SILU},
         {res_flat, HGEMMXetla_INT4::EpilogueType::RES_MUL}},
        &out,
        g_idx);
    return resize_as_mat1(input, out);
  } else {
    Attr attr;
#ifndef USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::woq_matmul_silu_mul(
        out,
        input,
        weight,
        weight_scl,
        weight_zp,
        res,
        group_size,
        false,
        attr,
        g_idx);
#else // USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_silu_mul(
        out,
        input,
        weight,
        std::nullopt,
        weight_scl,
        weight_zp,
        res,
        group_size,
        false,
        g_idx);
#endif // USE_PRIMITIVE_CACHE
    return out;
  }
}

static Tensor mm_bias_silu_mul_int4(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& bias,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t group_size,
    const Tensor& res,
    const c10::optional<Tensor>& g_idx) {
  Tensor out;
  if (choose_recommand_compute_eng()) {
    auto res_flat = res.flatten(0, -2);
    auto bias_flat = bias.flatten();
    auto dispatcher = mm_int4_dispatch(
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        {{bias_flat, HGEMMXetla_INT4::EpilogueType::BIAS},
         {Tensor(), HGEMMXetla_INT4::EpilogueType::SILU},
         {res_flat, HGEMMXetla_INT4::EpilogueType::RES_MUL}},
        &out,
        g_idx);
    return resize_as_mat1(input, out);
  } else {
#ifndef USE_PRIMITIVE_CACHE
    Attr attr;
    torch_ipex::xpu::oneDNN::woq_matmul_bias_silu_mul_int4(
        out,
        input,
        weight,
        weight_scl,
        weight_zp,
        res,
        group_size,
        false,
        attr,
        g_idx,
        bias);
#else // USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_bias_silu_mul(
        out,
        input,
        weight,
        bias,
        weight_scl,
        weight_zp,
        res,
        group_size,
        false,
        g_idx);
#endif // USE_PRIMITIVE_CACHE
    return out;
  }
}

static Tensor mm_add_int4(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t group_size,
    const Tensor& res,
    const c10::optional<Tensor>& g_idx) {
  Tensor out;
  if (choose_recommand_compute_eng()) {
    auto res_flat = res.flatten(0, -2);
    auto dispatcher = mm_int4_dispatch(
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        {{res_flat, HGEMMXetla_INT4::EpilogueType::RES_ADD}},
        &out,
        g_idx);
    return resize_as_mat1(input, out);
  } else {
    Attr attr;
#ifndef USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::woq_matmul_add_int4(
        out,
        input,
        weight,
        weight_scl,
        weight_zp,
        res,
        group_size,
        false,
        attr,
        g_idx);
#else // USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_add(
        out,
        input,
        weight,
        std::nullopt,
        weight_scl,
        weight_zp,
        res,
        group_size,
        false,
        g_idx);
#endif // USE_PRIMITIVE_CACHE
    return out;
  }
}

static Tensor mm_bias_add_int4(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& bias,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t group_size,
    const Tensor& res,
    const c10::optional<Tensor>& g_idx) {
  Tensor out;
  if (choose_recommand_compute_eng()) {
    auto res_flat = res.flatten(0, -2);
    auto bias_flat = bias.flatten();
    auto dispatcher = mm_int4_dispatch(
        input,
        weight,
        weight_scl,
        weight_zp,
        group_size,
        {{bias_flat, HGEMMXetla_INT4::EpilogueType::BIAS},
         {res_flat, HGEMMXetla_INT4::EpilogueType::RES_ADD}},
        &out,
        g_idx);
    return resize_as_mat1(input, out);
  } else {
#ifndef USE_PRIMITIVE_CACHE
    Attr attr;
    torch_ipex::xpu::oneDNN::woq_matmul_bias_add_int4(
        out,
        input,
        weight,
        weight_scl,
        weight_zp,
        res,
        group_size,
        false,
        attr,
        g_idx,
        bias);
#else // USE_PRIMITIVE_CACHE
    torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_bias_add(
        out,
        input,
        weight,
        bias,
        weight_scl,
        weight_zp,
        res,
        group_size,
        false,
        g_idx);
#endif // USE_PRIMITIVE_CACHE
    return out;
  }
}

static Tensor mm_w4a8(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t act_quant_mode,
    int64_t group_size,
    const c10::optional<Tensor>& g_idx) {
  TORCH_CHECK(
      act_quant_mode != -1,
      "Expect per-tensor or per-token quantization for activation but got ",
      act_quant_mode);
  Tensor out;
#ifndef USE_PRIMITIVE_CACHE
  TORCH_CHECK(false, "mm_w4a8 is only availble when USE_PRIMITIVE_CACHE=ON");
#else // USE_PRIMITIVE_CACHE
  torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16(
      out,
      input,
      weight,
      std::nullopt,
      weight_scl,
      weight_zp,
      group_size,
      false,
      g_idx,
      act_quant_mode);
#endif // USE_PRIMITIVE_CACHE
  return out;
}

static Tensor mm_bias_w4a8(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& bias_,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t act_quant_mode,
    int64_t group_size,
    const c10::optional<Tensor>& g_idx) {
  TORCH_CHECK(
      act_quant_mode != -1,
      "Expect per-tensor or per-token quantization for activation but got ",
      act_quant_mode);
  Tensor out;
#ifndef USE_PRIMITIVE_CACHE
  TORCH_CHECK(
      false, "mm_bias_w4a8 is only availble when USE_PRIMITIVE_CACHE=ON");
#else // USE_PRIMITIVE_CACHE
  torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16(
      out,
      input,
      weight,
      bias_,
      weight_scl,
      weight_zp,
      group_size,
      false,
      g_idx,
      act_quant_mode);
#endif // USE_PRIMITIVE_CACHE
  return out;
}

static Tensor mm_add_w4a8(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t act_quant_mode,
    int64_t group_size,
    const Tensor& res,
    const c10::optional<Tensor>& g_idx) {
  TORCH_CHECK(
      act_quant_mode != -1,
      "Expect per-tensor or per-token quantization for activation but got ",
      act_quant_mode);
  Tensor out;
#ifndef USE_PRIMITIVE_CACHE
  TORCH_CHECK(
      false, "mm_add_w4a8 is only availble when USE_PRIMITIVE_CACHE=ON");
#else // USE_PRIMITIVE_CACHE
  torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_add(
      out,
      input,
      weight,
      std::nullopt,
      weight_scl,
      weight_zp,
      res,
      group_size,
      false,
      g_idx,
      act_quant_mode);
#endif // USE_PRIMITIVE_CACHE
  return out;
}

static Tensor mm_bias_add_w4a8(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& bias,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t act_quant_mode,
    int64_t group_size,
    const Tensor& res,
    const c10::optional<Tensor>& g_idx) {
  TORCH_CHECK(
      act_quant_mode != -1,
      "Expect per-tensor or per-token quantization for activation but got ",
      act_quant_mode);
  Tensor out;
#ifndef USE_PRIMITIVE_CACHE
  TORCH_CHECK(
      false, "mm_bias_add_w4a8 is only availble when USE_PRIMITIVE_CACHE=ON");
#else // USE_PRIMITIVE_CACHE
  torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_bias_add(
      out,
      input,
      weight,
      bias,
      weight_scl,
      weight_zp,
      res,
      group_size,
      false,
      g_idx,
      act_quant_mode);
#endif // USE_PRIMITIVE_CACHE
  return out;
}

static Tensor mm_silu_mul_w4a8(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t act_quant_mode,
    int64_t group_size,
    const Tensor& res,
    const c10::optional<Tensor>& g_idx) {
  TORCH_CHECK(
      act_quant_mode != -1,
      "Expect per-tensor or per-token quantization for activation but got ",
      act_quant_mode);
  Tensor out;
#ifndef USE_PRIMITIVE_CACHE
  TORCH_CHECK(
      false, "mm_silu_mul_w4a8 is only availble when USE_PRIMITIVE_CACHE=ON");
#else // USE_PRIMITIVE_CACHE
  torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_silu_mul(
      out,
      input,
      weight,
      std::nullopt,
      weight_scl,
      weight_zp,
      res,
      group_size,
      false,
      g_idx,
      act_quant_mode);
#endif // USE_PRIMITIVE_CACHE
  return out;
}

static Tensor mm_bias_silu_mul_w4a8(
    const Tensor& input,
    const Tensor& weight,
    const Tensor& bias,
    const Tensor& weight_scl,
    const Tensor& weight_zp,
    int64_t act_quant_mode,
    int64_t group_size,
    const Tensor& res,
    const c10::optional<Tensor>& g_idx) {
  TORCH_CHECK(
      act_quant_mode != -1,
      "Expect per-tensor or per-token quantization for activation but got ",
      act_quant_mode);
  Tensor out;
#ifndef USE_PRIMITIVE_CACHE
  TORCH_CHECK(
      false,
      "mm_bias_silu_mul_w4a8 is only availble when USE_PRIMITIVE_CACHE=ON");
#else // USE_PRIMITIVE_CACHE
  torch_ipex::xpu::oneDNN::dnnl_matmul_w4a16_and_bias_silu_mul(
      out,
      input,
      weight,
      bias,
      weight_scl,
      weight_zp,
      res,
      group_size,
      false,
      g_idx,
      act_quant_mode);
#endif // USE_PRIMITIVE_CACHE
  return out;
}

} // namespace AtenIpexTypeXPU
} // namespace at

namespace {
IPEX_LIBRARY_FRAGMENT() {
  IPEX_OP_REGISTER(
      "mm_qkv_out_int4.xpu", at::AtenIpexTypeXPU::mm_qkv_out_wint4);
  IPEX_OP_REGISTER("mm_qkv_int4.xpu", at::AtenIpexTypeXPU::mm_qkv_wint4);
  IPEX_OP_REGISTER(
      "mlp_silu_mul_out_int4.xpu", at::AtenIpexTypeXPU::mlp_silu_mul_out_int4);
  IPEX_OP_REGISTER(
      "mlp_silu_mul_int4.xpu", at::AtenIpexTypeXPU::mlp_silu_mul_int4);
  IPEX_OP_REGISTER(
      "mlp_bias_silu_mul_out_int4.xpu",
      at::AtenIpexTypeXPU::mlp_bias_silu_mul_out_int4);
  IPEX_OP_REGISTER(
      "mlp_bias_silu_mul_int4.xpu",
      at::AtenIpexTypeXPU::mlp_bias_silu_mul_int4);
  IPEX_OP_REGISTER(
      "mlp_silu_mul_bias_out_int4.xpu",
      at::AtenIpexTypeXPU::mlp_silu_mul_bias_out_int4);
  IPEX_OP_REGISTER(
      "mlp_silu_mul_bias_int4.xpu",
      at::AtenIpexTypeXPU::mlp_silu_mul_bias_int4);
  IPEX_OP_REGISTER(
      "mlp_bias_silu_mul_bias_out_int4.xpu",
      at::AtenIpexTypeXPU::mlp_bias_silu_mul_bias_out_int4);
  IPEX_OP_REGISTER(
      "mlp_bias_silu_mul_bias_int4.xpu",
      at::AtenIpexTypeXPU::mlp_bias_silu_mul_bias_int4);
  IPEX_OP_REGISTER("mm_int4.xpu", at::AtenIpexTypeXPU::mm_int4);
  IPEX_OP_REGISTER("mm_int4_out.xpu", at::AtenIpexTypeXPU::mm_int4_out);
  IPEX_OP_REGISTER("mm_bias_int4.xpu", at::AtenIpexTypeXPU::mm_bias_int4);
  IPEX_OP_REGISTER("mm_silu_int4.xpu", at::AtenIpexTypeXPU::mm_silu_int4);
  IPEX_OP_REGISTER("mm_resmul_int4.xpu", at::AtenIpexTypeXPU::mm_resmul_int4);
  IPEX_OP_REGISTER(
      "mm_bias_gelu_int4.xpu", at::AtenIpexTypeXPU::mm_bias_gelu_int4);
  IPEX_OP_REGISTER(
      "mm_bias_resadd_resadd_int4.xpu",
      at::AtenIpexTypeXPU::mm_bias_resadd_resadd_int4);
  IPEX_OP_REGISTER("mm_low_bits.xpu", at::AtenIpexTypeXPU::mm_low_bits);
  IPEX_OP_REGISTER(
      "mm_silu_mul_int4.xpu", at::AtenIpexTypeXPU::mm_silu_mul_int4);
  IPEX_OP_REGISTER(
      "mm_bias_silu_mul_int4.xpu", at::AtenIpexTypeXPU::mm_bias_silu_mul_int4);
  IPEX_OP_REGISTER("mm_add_int4.xpu", at::AtenIpexTypeXPU::mm_add_int4);
  IPEX_OP_REGISTER(
      "mm_bias_add_int4.xpu", at::AtenIpexTypeXPU::mm_bias_add_int4);
  IPEX_OP_REGISTER("mm_w4a8.xpu", at::AtenIpexTypeXPU::mm_w4a8);
  IPEX_OP_REGISTER("mm_bias_w4a8.xpu", at::AtenIpexTypeXPU::mm_bias_w4a8);
  IPEX_OP_REGISTER("mm_add_w4a8.xpu", at::AtenIpexTypeXPU::mm_add_w4a8);
  IPEX_OP_REGISTER(
      "mm_bias_add_w4a8.xpu", at::AtenIpexTypeXPU::mm_bias_add_w4a8);
  IPEX_OP_REGISTER(
      "mm_silu_mul_w4a8.xpu", at::AtenIpexTypeXPU::mm_silu_mul_w4a8);
  IPEX_OP_REGISTER(
      "mm_bias_silu_mul_w4a8.xpu", at::AtenIpexTypeXPU::mm_bias_silu_mul_w4a8);
}
} // namespace
#else
#include <ATen/ATen.h>
#include "utils/CustomOperatorRegistration.h"

namespace at {
namespace AtenIpexTypeXPU {

Tensor _weight_int4pack_mm(
    const Tensor& A,
    const Tensor& B,
    int64_t qGroupSize,
    const Tensor& qScaleAndZeros) {
  TORCH_CHECK(false, "_weight_int4pack_mm is not supported without XeTLA");
}

} // namespace AtenIpexTypeXPU
} // namespace at
#endif
