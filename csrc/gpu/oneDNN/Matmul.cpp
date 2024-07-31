#if __has_include(<sycl/sycl.hpp>)
#include <sycl/sycl.hpp>
#elif __has_include(<CL/sycl.hpp>)
#include <CL/sycl.hpp>
#else
#error "Unsupported compiler"
#endif

#include <ATen/ATen.h>

#include <ATen/record_function.h>

#include <oneDNN/Reorder.h>
#include <oneDNN/Runtime.h>
#include <quantized/QUtils.h>
#include <runtime/Utils.h>
#include <tensor/Tensor.h>
#include <utils/LRUCache.h>
#include "Attr.h"
#include "Utils.h"

#include <oneapi/dnnl/dnnl.hpp>

using namespace dnnl;
using namespace torch_ipex::xpu::dpcpp;
using namespace at::AtenIpexTypeXPU;

namespace torch_ipex::xpu {
namespace oneDNN {
sycl::event matmul(
    Tensor& result,
    const Tensor& mat1,
    const Tensor& mat2,
    const Tensor& b_raw,
    bool m2_trans,
    Attr attr,
    const std::vector<sycl::event>& deps) {
  size_t dims = result.dim();
  TORCH_CHECK(
      dims == 2 || dims == 3,
      "oneDNN matmul only works with 2D or 3D, got ",
      dims);
  TORCH_CHECK(
      dims == mat1.dim() && dims == mat2.dim(),
      "oneDNN input matrixes must have the same ranks");
  TORCH_CHECK(result.defined(), "oneDNN matmul result should be defined");

  auto dtype = mat1.scalar_type();
  if (dtype == at::ScalarType::Int || dtype == at::ScalarType::Long ||
      dtype == at::ScalarType::Short) {
    AT_ERROR("\"oneDNN matmul\" not implemented for '", toString(dtype), "'");
  }

  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  // engine index means the engine created on which device
  auto engine_index = curDevice.index();
  auto strm = GpuStreamManager::Instance().get_stream();

  Tensor m1 = torch_ipex::xpu::oneDNN::is_onednn_matmul_strides(mat1)
      ? mat1
      : mat1.contiguous();
  Tensor m2 = torch_ipex::xpu::oneDNN::is_onednn_matmul_strides(mat2)
      ? mat2
      : mat2.contiguous();
  Tensor dst = torch_ipex::xpu::oneDNN::is_onednn_matmul_strides(result, true)
      ? result
      : result.contiguous();

  int64_t m = dst.size(-2);
  int64_t n = dst.size(-1);
  int64_t k = m1.size(-1);
  int64_t mb = 1;

  if (dims == 3) {
    mb = dst.size(0);
    TORCH_CHECK(
        mb == m1.size(0) && mb == m2.size(0),
        "batch size mismatch, dst mb: ",
        mb,
        "m1 mb",
        m1.size(0),
        " m2 mb: ",
        m2.size(0));
  }

  // validate bias and make it compatible with oneDNN implementation
  bool with_bias = false;
  Tensor b = b_raw;
  if (b.defined()) {
    with_bias = true;
    if (b.dim() == 1) {
      TORCH_CHECK(
          b.size(0) == n || b.size(0) == 1,
          "matmul supports [n] or [1] when bias dim is 1 ...");
      if (b.size(0) == 0) {
        with_bias = false;
      } else if (m1.dim() == 3) {
        b = b.expand({mb, m, n}).contiguous();
      } else if (m1.dim() == 2) {
        b = b.expand({1, n}).contiguous();
      }
    } else if (b.dim() == 2) {
      TORCH_CHECK(
          (b.size(0) == m && b.size(1) == n) ||
              (b.size(0) == 1 && b.size(1) == n) ||
              (b.size(0) == m && b.size(1) == 1) ||
              (b.size(0) == 1 && b.size(1) == 1),
          "matmul supports [m, n] or [1, n] or [m, 1] or [1, 1] when bias dim is 2 ...");
      if (b.size(0) == 1 && b.size(1) == 1)
        b = b.expand({1, n}).contiguous();
    } else if (b.dim() == 3) {
      TORCH_CHECK(
          are_expandable({mb, m, n}, b.sizes()),
          "matmul bias must be expandable to:",
          dst.sizes(),
          " but got:",
          b.sizes());
      b = b.expand({mb, m, n}).contiguous();
    } else if (b.dim() == 0) {
      TORCH_CHECK(
          b.numel() == 1, "matmul supports 1 numel when bias dim is [] ...");
      if (m1.dim() == 3) {
        b = b.expand({mb, m, n}).contiguous();
      } else {
        b = b.expand({1, n}).contiguous();
      }
    } else {
      TORCH_CHECK(0, "unsupported bias dim in matmul ...");
    }
  }

  // bias is fused in post-op for quantized path
  with_bias &= (!m1.is_quantized()) && (!m2.is_quantized());
  b = b.contiguous(); // avoid reorder 2 times

  // ipex matmul support both ab/ba shape for m2 tensor, we don't check any more
  auto m1_usr_dt = get_onednn_dtype_include_double(m1);
  auto m2_usr_dt = get_onednn_dtype_include_double(m2);
  auto dst_usr_dt = get_onednn_dtype_include_double(dst);

  auto m1_dt = m1_usr_dt;
  auto m2_dt = m2_usr_dt;
  auto dst_dt = dst_usr_dt;
  memory::data_type bias_dt;

  memory::desc m1_md, m1_usr_md, m1_any_md;
  memory::desc m2_md, m2_usr_md, m2_any_md;
  memory::desc dst_md, dst_usr_md, dst_any_md;
  memory::desc bias_md;

  // Naive Master weight
  if (m1_dt == memory::data_type::bf16 && m2_dt == memory::data_type::f32) {
    m2_dt = memory::data_type::bf16;
    dst_dt = memory::data_type::bf16;
  } else if (
      m1_dt == memory::data_type::f32 && m2_dt == memory::data_type::bf16) {
    m1_dt = memory::data_type::bf16;
    dst_dt = memory::data_type::bf16;
  } else if (
      m1_dt == memory::data_type::f8_e4m3 ||
      m1_dt == memory::data_type::f8_e5m2) {
    dst_dt = memory::data_type::f32;
    dst_usr_dt = memory::data_type::f32;
    if (with_bias) {
      b = b.to(at::kFloat);
    }
    result = result.to(at::kFloat);
    // TODO: refine code to remove unnecessary datatype conversion for better
    // performance.
    dst = dst.to(at::kFloat);
  }

  memory::dims m1_dims, m2_dims, dst_dims, bias_dims;
  memory::dims m1_strides, m2_strides, dst_strides, bias_strides;
  if (dims == 2) {
    m1_dims = {m, k};
    m2_dims = {k, n};
    dst_dims = {m, n};

    m1_strides = {m1.stride(0), m1.stride(1)};
    if (m2_trans) {
      m2_strides = {m2.stride(0), m2.stride(1)};
    } else {
      m2_strides = {m2.stride(1), m2.stride(0)};
    }
    dst_strides = {dst.stride(0), dst.stride(1)};
  } else {
    m1_dims = {mb, m, k};
    m2_dims = {mb, k, n};
    dst_dims = {mb, m, n};

    m1_strides = {m1.stride(0), m1.stride(1), m1.stride(2)};
    if (m2_trans) {
      m2_strides = {m2.stride(0), m2.stride(1), m2.stride(2)};
    } else {
      m2_strides = {m2.stride(0), m2.stride(2), m2.stride(1)};
    }
    dst_strides = {dst.stride(0), dst.stride(1), dst.stride(2)};
  }

  if (with_bias) {
    bias_dims = get_onednn_dims(b);
    bias_dt = get_onednn_dtype_include_double(b);
    bias_strides = get_onednn_strides(b);
  }

  auto is_onednn_layout_suggested = using_onednn_layout_for_matmul(m1);
  auto use_deterministic = globalContext().deterministicAlgorithms() ||
      at::globalContext().deterministicMkldnn();

  post_ops po;
  attr.extract_post_ops(po, dst);

  lru_key_t key_primitive;
#ifdef USE_PRIMITIVE_CACHE
  create_key(
      key_primitive,
      engine_index,
      m1_dims,
      m2_dims,
      dst_dims,
      bias_dims,
      m1_dt,
      m2_dt,
      dst_dt,
      bias_dt,
      m1_strides,
      m2_strides,
      dst_strides,
      bias_strides,
      dims,
      is_onednn_layout_suggested,
      attr,
      use_deterministic);
#endif

  std::unordered_map<int, memory> args;

  dnnl::matmul matmul_p;
  dnnl::matmul::primitive_desc matmul_pd;

#ifdef USE_PRIMITIVE_CACHE
  bool load_from_cache = find_key<dnnl::matmul>(key_primitive);
#else
  bool load_from_cache = false;
#endif
  if (load_from_cache) {
    // load primitive from cache
    matmul_p = fetch_m<dnnl::matmul>(key_primitive);
    auto matmul_pd_t = matmul_p.get_primitive_desc();
    matmul_pd = dnnl::matmul::primitive_desc(
        const_cast<dnnl_primitive_desc_t>(matmul_pd_t));
  } else {
    // STEP1: create memory desc
    if (dims == 2 && is_onednn_layout_suggested) {
      m1_md = memory::desc(m1_dims, m1_dt, memory::format_tag::any);
      m2_md = memory::desc(m2_dims, m2_dt, memory::format_tag::any);
      dst_md = memory::desc(dst_dims, dst_dt, memory::format_tag::any);
    } else {
      m1_md = memory::desc(m1_dims, m1_dt, m1_strides);
      m2_md = memory::desc(m2_dims, m2_dt, m2_strides);
      dst_md = memory::desc(dst_dims, dst_dt, dst_strides);
    }

    // STEP2: creat attribute
    primitive_attr pattr;
    pattr.set_post_ops(po);
    if (globalContext().deterministicAlgorithms() ||
        at::globalContext().deterministicMkldnn())
      pattr.set_deterministic(true);

#ifdef USE_SCRATCHPAD_MODE
    pattr.set_scratchpad_mode(dnnl::scratchpad_mode::user);
#endif

    if (m1_dt == memory::data_type::f32) {
      pattr.set_fpmath_mode(torch_ipex::xpu::oneDNN::get_onednn_fpmath_mode());
    }

    // STEP3: create primitive
    if (with_bias) {
      bias_md = memory::desc(bias_dims, bias_dt, bias_strides);
      matmul_pd =
          matmul::primitive_desc(engine, m1_md, m2_md, bias_md, dst_md, pattr);
    } else {
      matmul_pd = matmul::primitive_desc(engine, m1_md, m2_md, dst_md, pattr);
    }

#ifdef USE_PRIMITIVE_CACHE
    matmul_p = create_and_fetch_m<dnnl::matmul>(key_primitive, matmul_pd);
#else
    matmul_p = dnnl::matmul(matmul_pd);
#endif
  }

  m1_usr_md = memory::desc(m1_dims, m1_usr_dt, m1_strides);
  m2_usr_md = memory::desc(m2_dims, m2_usr_dt, m2_strides);
  dst_usr_md = memory::desc(dst_dims, dst_usr_dt, dst_strides);

  // STEP4: create memory
  auto m1_ctx = at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(m1);
  auto m1_usr_m = m1_ctx.is_plain()
      ? dpcpp_onednn_memory(m1_usr_md, engine, m1.data_ptr())
      : dpcpp_onednn_memory({m1_ctx.meta()}, engine, m1.data_ptr());

  auto m2_ctx = at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(m2);
  auto m2_usr_m = m2_ctx.is_plain()
      ? dpcpp_onednn_memory(m2_usr_md, engine, m2.data_ptr())
      : dpcpp_onednn_memory({m2_ctx.meta()}, engine, m2.data_ptr());

  auto dst_ctx = at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(dst);
  auto dst_usr_m = dst_ctx.is_plain()
      ? dpcpp_onednn_memory(dst_usr_md, engine, dst.data_ptr())
      : dpcpp_onednn_memory({dst_ctx.meta()}, engine, dst.data_ptr());

  auto expected_m1_md = matmul_pd.src_desc();
  auto expected_m2_md = matmul_pd.weights_desc();
  auto expected_dst_md = matmul_pd.dst_desc();

  memory m1_m = m1_usr_m, m2_m = m2_usr_m, dst_m = dst_usr_m;
  Tensor m1_, m2_, dst_;

  auto weight_cache_optimization = [&]() {
    bool onoff = false;
    onoff |= is_onednn_layout_suggested;
    onoff &= c10::InferenceMode::is_enabled();
    return onoff;
  }();

  // reorder cases
  // case1: master weight support to reorder data type
  // case2: block format support to reorder format
  if (m1_usr_m.get_desc() != expected_m1_md) {
    m1_ = empty_opaque_tensor(expected_m1_md, m1.options(), c10::nullopt);
    m1_m = dpcpp_onednn_memory(expected_m1_md, engine, m1_.data_ptr());
    torch_ipex::xpu::oneDNN::reorder(m1, m1_);
  }

  if (m2_usr_m.get_desc() != expected_m2_md) {
    m2_ = empty_opaque_tensor(expected_m2_md, m2.options(), c10::nullopt);
    m2_m = dpcpp_onednn_memory(expected_m2_md, engine, m2_.data_ptr());
    auto m2_onednn_matmul_shape_compatible = m2_trans ? m2 : m2.t();
    torch_ipex::xpu::oneDNN::reorder(m2_onednn_matmul_shape_compatible, m2_);

    if (weight_cache_optimization) {
      auto ctx_ =
          at::AtenIpexTypeXPU::DPCPPTensorContext::release_tensor_ctx(m2_);
      // assume oneDNN.matmul.weight is the permution of torch.nn.Linear.weight
      ctx_.set_aten_meta(
          {m2_onednn_matmul_shape_compatible.sizes().vec(),
           m2_onednn_matmul_shape_compatible.strides().vec()});
      at::AtenIpexTypeXPU::DPCPPTensorContext::set_tensor_ctx(
          m2, std::move(ctx_));
    }
  }

  // bias add for gen12hp platform
  if (dst_usr_m.get_desc() != expected_dst_md) {
    dst_ = empty_opaque_tensor(expected_dst_md, dst.options(), c10::nullopt);
    dst_m = dpcpp_onednn_memory(expected_dst_md, engine, dst_.data_ptr());
    if (attr.with_sum())
      torch_ipex::xpu::oneDNN::reorder(dst, dst_);
  }
  if (attr.with_binary())
    attr.construct_post_binary(matmul_pd, args);

#ifdef USE_SCRATCHPAD_MODE
  size_t scratchpad_size = matmul_pd.scratchpad_desc().get_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, m1.options().dtype(at::kByte), c10::nullopt);
  auto scratchpad_memory = dpcpp_onednn_memory(
      matmul_pd.scratchpad_desc(), engine, scratchpad_tensor.data_ptr());
  args.insert({DNNL_ARG_SCRATCHPAD, scratchpad_memory});
#endif

  args.insert({DNNL_ARG_SRC, m1_m});
  args.insert({DNNL_ARG_WEIGHTS, m2_m});
  args.insert({DNNL_ARG_DST, dst_m});
  if (with_bias) {
    auto bias_m = dpcpp_onednn_memory(bias_md, engine, b.data_ptr());
    args.insert({DNNL_ARG_BIAS, bias_m});
  }

  sycl::event output_event;
  DPCPP_ONEDNN_EXEC_WITH_EVENT(matmul_p, strm, args, deps, output_event);

  if (is_onednn_layout_suggested && dst_m != dst_usr_m && dims == 2) {
    auto blk_ctx = DPCPPTensorContext::release_tensor_ctx(dst_);
    DPCPPTensorContext::set_tensor_ctx(dst, std::move(blk_ctx));
  }

  if (!dst.is_same(result))
    result.copy_(dst);

  return output_event;
}

sycl::event dot_matmul(Tensor& result, const Tensor& mat1, const Tensor mat2) {
  at::Device curDevice = at::Device(at::kXPU, at::xpu::current_device());
  auto engine = xpu::oneDNN::GpuEngineManager::Instance().get_engine(curDevice);
  auto engine_index = curDevice.index();
  auto strm = xpu::oneDNN::GpuStreamManager::Instance().get_stream();

  Tensor m1 =
      xpu::oneDNN::is_onednn_matmul_strides(mat1) ? mat1 : mat1.contiguous();
  Tensor m2 =
      xpu::oneDNN::is_onednn_matmul_strides(mat2) ? mat2 : mat2.contiguous();
  Tensor dst = xpu::oneDNN::is_onednn_matmul_strides(result, true)
      ? result
      : result.contiguous();

  const auto M = 1;
  const auto K = mat1.sizes()[1];
  const auto N = 1;

  auto m1_dt = xpu::oneDNN::get_onednn_dtype_include_double(m1);
  auto m2_dt = xpu::oneDNN::get_onednn_dtype_include_double(m2);
  auto dst_dt = xpu::oneDNN::get_onednn_dtype_include_double(dst);

  memory::dims m1_dims = {M, K};
  memory::dims m2_dims = {K, N};
  memory::dims dst_dims = {M, N};

  memory::dims m1_strides = {m1.stride(0), m1.stride(1)};
  memory::dims m2_strides = {m2.stride(0), m2.stride(1)};
  memory::dims dst_strides = {1, 1};

  auto m1_md = memory::desc(m1_dims, m1_dt, m1_strides);
  auto m2_md = memory::desc(m2_dims, m2_dt, m2_strides);
  auto dst_md = memory::desc(dst_dims, dst_dt, dst_strides);

  auto m1_usr_md = m1_md;
  auto m2_usr_md = m2_md;
  auto dst_usr_md = dst_md;

  primitive_attr pattr;
  dnnl::matmul::primitive_desc matmul_pd =
      matmul::primitive_desc(engine, m1_md, m2_md, dst_md, pattr);
  std::unordered_map<int, memory> args;
  dnnl::matmul matmul_p = dnnl::matmul(matmul_pd);

  auto m1_usr_m =
      xpu::oneDNN::dpcpp_onednn_memory(m1_usr_md, engine, m1.data_ptr());
  auto m2_usr_m =
      xpu::oneDNN::dpcpp_onednn_memory(m2_usr_md, engine, m2.data_ptr());
  auto dst_usr_m =
      xpu::oneDNN::dpcpp_onednn_memory(dst_usr_md, engine, dst.data_ptr());

  args.insert({DNNL_ARG_SRC, m1_usr_m});
  args.insert({DNNL_ARG_WEIGHTS, m2_usr_m});
  args.insert({DNNL_ARG_DST, dst_usr_m});

  std::vector<sycl::event> deps;
  sycl::event output_event;
  DPCPP_ONEDNN_EXEC_WITH_EVENT(matmul_p, strm, args, deps, output_event);

  return output_event;
}

} // namespace oneDNN
} // namespace torch_ipex::xpu
