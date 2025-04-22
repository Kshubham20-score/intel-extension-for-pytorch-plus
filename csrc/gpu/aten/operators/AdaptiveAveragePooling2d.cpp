#include <ATen/ATen.h>
#include <ATen/Config.h>
#include <ATen/NativeFunctions.h>
#include <ATen/OpMathType.h>
#include <ATen/native/AdaptivePooling.h>
#include <ATen/native/Pool.h>

#include <oneDNN/oneDNN.h>
#include <quantized/QUtils.h>
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"

#include <ATen/Context.h>
#include <runtime/Utils.h>
#include <tensor/TensorMeta.h>
#include <utils/oneMKLUtils.h>

#include <ATen/DeviceGuard.h>
#include <ATen/core/op_registration/adaption.h>
#include "utils/CustomOperatorRegistration.h"

#include <torch/custom_class.h>
#include "comm/ParamUtils.h"

#include <vector>

using namespace dnnl;
using namespace torch_ipex::xpu::dpcpp;
using namespace torch_ipex::xpu::oneDNN;

namespace {

template <
    typename scalar_t,
    typename accscalar_t,
    bool is_channels_last,
    bool is_quantized = false>
struct AdaptiveAvgPool2dKernelFunctor {
  void operator()(sycl::nd_item<1> item) const {
    int64_t gi = item.get_global_linear_id();
    for (int64_t i = gi; i < numel; i += global_range) {
      int64_t _ow, _oh, _oc, _ob;
      if constexpr (is_channels_last) {
        _oc = i % oc;
        _ow = i / oc % ow;
        _oh = i / oc / ow % oh;
        _ob = i / oc / ow / oh;
      } else {
        _ow = i % ow;
        _oh = i / ow % oh;
        _oc = i / ow / oh % oc;
        _ob = i / ow / oh / oc;
      }

      int64_t _ih0 = native::start_index(_oh, oh, ih);
      int64_t _ih1 = native::end_index(_oh, oh, ih);
      int64_t _iw0 = native::start_index(_ow, ow, iw);
      int64_t _iw1 = native::end_index(_ow, ow, iw);
      int64_t kh = _ih1 - _ih0;
      int64_t kw = _iw1 - _iw0;
      int64_t _ib = _ob;
      int64_t _ic = _oc;

      accscalar_t sum = static_cast<accscalar_t>(0);
      for (int _ih = _ih0; _ih < _ih1; _ih++) {
        for (int _iw = _iw0; _iw < _iw1; _iw++) {
          if constexpr (is_quantized) {
            sum += accscalar_t(
                ((accscalar_t)input[_ib][_ic][_ih][_iw] - (accscalar_t)zp) *
                scale);
          } else {
            sum += accscalar_t(input[_ib][_ic][_ih][_iw]);
          }
        }
      }
      accscalar_t avg = sum / kh / kw;

      const auto store = [](PackedTensorAccessor64<scalar_t, 4> oacc,
                            int64_t _ob,
                            int64_t _oc,
                            int64_t _oh,
                            int64_t _ow,
                            scalar_t res) { oacc[_ob][_oc][_oh][_ow] = res; };
      if constexpr (is_quantized) {
        scalar_t qavg = quantize_val<scalar_t>(scale, zp, avg);
        store(output, _ob, _oc, _oh, _ow, qavg);
      } else {
        store(output, _ob, _oc, _oh, _ow, avg);
      }
    }
  }
  AdaptiveAvgPool2dKernelFunctor(
      int ih_,
      int iw_,
      int ob_,
      int oc_,
      int oh_,
      int ow_,
      accscalar_t scale_,
      int64_t zp_,
      int64_t numel_,
      int global_range_,
      PackedTensorAccessor64<scalar_t, 4> input_,
      PackedTensorAccessor64<scalar_t, 4> output_)
      : ih(ih_),
        iw(iw_),
        ob(ob_),
        oc(oc_),
        oh(oh_),
        ow(ow_),
        scale(scale_),
        zp(zp_),
        numel(numel_),
        global_range(global_range_),
        input(input_),
        output(output_) {}

 private:
  int ih;
  int iw;
  int ob;
  int oc;
  int oh;
  int ow;
  accscalar_t scale;
  int64_t zp;
  int64_t numel;
  int global_range;
  PackedTensorAccessor64<scalar_t, 4> input;
  PackedTensorAccessor64<scalar_t, 4> output;
};

template <
    typename scalar_t,
    typename accscalar_t,
    bool is_channels_last,
    bool is_quantized = false>
void adaptive_avg_pool2d_kernel(
    PackedTensorAccessor64<scalar_t, 4> input,
    PackedTensorAccessor64<scalar_t, 4> output,
    std::tuple<accscalar_t, int64_t> quantizer = {1, 0}) {
  int ih = input.size(2);
  int iw = input.size(3);
  int ob = output.size(0);
  int oc = output.size(1);
  int oh = output.size(2);
  int ow = output.size(3);

  accscalar_t scale = std::get<0>(quantizer);
  int64_t zp = std::get<1>(quantizer);

  int64_t numel = ob * oc * oh * ow;
  int total_item = std::min(numel, dpcppMaxWorkItemsPerTile());
  int local_range = dpcppMaxWorkItemsPerEU();
  int global_range = total_item < local_range
      ? local_range
      : (total_item / local_range) * local_range;
  auto cgf = DPCPP_Q_CGF(cgh) {
    AdaptiveAvgPool2dKernelFunctor<
        scalar_t,
        accscalar_t,
        is_channels_last,
        is_quantized>
        kfn(ih,
            iw,
            ob,
            oc,
            oh,
            ow,
            scale,
            zp,
            numel,
            global_range,
            input,
            output);

    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<1>(
            sycl::range<1>(global_range), sycl::range<1>(local_range)),
        kfn);
  };

  DPCPP_Q_SUBMIT(dpcppGetCurrentQueue(), cgf);
}

void adaptive_avg_pool2d_out_template(
    Tensor& output,
    const Tensor& input,
    IntArrayRef output_size) {
  int64_t ndim = input.dim();
  for (const auto i : {-2, -1}) {
    TORCH_CHECK(
        input.size(i) > 0,
        "adaptive_average_pool2d_dpcpp(): Expected input to have non-zero size for non-batch dimensions, "
        "but input has sizes ",
        input.sizes(),
        " with dimension ",
        i + ndim,
        " being "
        "empty");
  }

  TORCH_CHECK(
      (ndim == 3 || ndim == 4),
      "non-empty 3D or 4D (batch mode) tensor expected for input");

  TORCH_CHECK(
      output_size.size() == 2,
      "adaptive_average_pool2d: internal error: output_size.size() must be 2");

  auto outputWidth = output_size[1];
  auto outputHeight = output_size[0];

  if (!input.is_quantized() && outputWidth == 1 && outputHeight == 1) {
    // in this case, adaptive pooling is just computing mean over hw
    // dimensions, which can be done more efficiently

    output = input.mean({-1, -2}, /* keepdim = */ true);
    if (input.suggest_memory_format() == at::MemoryFormat::ChannelsLast) {
      // assert ndim == 4, since ndim = 3 doesn't give channels_last
      const int n = input.size(0);
      const int c = input.size(1);
      output.as_strided_({n, c, 1, 1}, {c, 1, c, c});
    }
    return;
  }

  /* sizes */
  const int64_t nbatch = input.ndimension() == 4 ? input.size(-4) : 1;
  const auto nInputPlane = input.size(-3);
  const auto inputHeight = input.size(-2);
  const auto inputWidth = input.size(-1);
  Tensor input_;
  if (input.ndimension() == 3) {
    input_ = input.contiguous();
    output.resize_({nInputPlane, outputHeight, outputWidth});
  } else {
    auto smf = input.suggest_memory_format();
    input_ = contiguous_if_needed(input, smf);
    output.resize_({nbatch, nInputPlane, outputHeight, outputWidth}, smf);
  }
  if (output.numel() == 0) {
    return;
  }
  int dH = std::floor((float)2 * inputHeight / outputHeight) -
      (inputHeight / outputHeight);
  int dW = std::floor((float)2 * inputWidth / outputWidth) -
      (inputWidth / outputWidth);
  std::vector<int64_t> stride_vec = {dH, dW};

  int kH = std::ceil((float)2 * inputHeight / outputHeight) -
      (inputHeight / outputHeight);
  int kW = std::ceil((float)2 * inputWidth / outputWidth) -
      (inputWidth / outputWidth);
  std::vector<int64_t> kernel_size_vec = {kH, kW};

  // per oneDNN definition, no dilation means dilation ratio is 0
  std::vector<int64_t> dilation_vec = {0, 0};

  int padH = (dH * (outputHeight - 1) + kH - inputHeight) / 2;
  int padW = (dW * (outputWidth - 1) + kW - inputWidth) / 2;
  std::vector<int64_t> padding_vec = {padH, padW};

  if (torch_ipex::xpu::oneDNN::is_valid_pooling(
          {inputHeight, inputWidth},
          {outputHeight, outputWidth},
          {kH, kW},
          {dH, dW},
          {padH, padW})) {
    /* PyTorch support two cases of AdaptiveAvgPool2d:
       1. 3D: Input (C, H, W),  Output (C, H0, W0), Kernel (kH, kW)
       This case does not support channel last format. For a 3-dim tensor,
       the suggest_memory_format can only be Contiguous or ChannelsLast1D
       (nwc), the ChannelsLast1D (nwc) does not match the sementics of Input
       (C, H, W) case. Then the suggest_memory_format can only be Contiguous.
       2. 4D: Input (N, C, H, W),  Output (N, C, H0, W0), Kernel (kH, kW)
       This case supports Contiguous and ChannelsLast2D memory_format. */
    torch_ipex::xpu::oneDNN::pooling<alg::pooling_avg_exclude_padding>(
        output,
        input_,
        nbatch,
        nInputPlane,
        0,
        inputHeight,
        inputWidth,
        0,
        outputHeight,
        outputWidth,
        stride_vec,
        kernel_size_vec,
        dilation_vec,
        padding_vec,
        padding_vec);
  } else {
    TORCH_CHECK(
        !is_opaque_u8(input),
        "XPU opaque u8 tensor is not supported in SYCL kernel ...");

    input_ = to_plain_if_needed(input_);

    bool is_3d = input_.ndimension() == 3;
    if (is_3d) {
      input_.resize_({1, nInputPlane, inputHeight, inputWidth});
      output.resize_({1, nInputPlane, outputHeight, outputWidth});
    }

    if (input_.is_quantized()) {
      float scale = input.scalar_type() == kQUInt8 ? input.q_scale() / 2.0f
                                                   : input.q_scale();

      IPEX_DISPATCH_QTYPE_ONLY_WITH_UNDERLYING(
          input_.scalar_type(), "aten::adpative_avg_pooled", 0, [&]() {
            auto iacc = input_.packed_accessor64<scalar_t_0, 4>();
            auto oacc = output.packed_accessor64<scalar_t_0, 4>();
            if (is_smf_channels_last(output)) {
              adaptive_avg_pool2d_kernel<scalar_t_0, float, true, true>(
                  iacc, oacc, {scale, 0 /* TODO: Asymm */});
            } else {
              adaptive_avg_pool2d_kernel<scalar_t_0, float, false, true>(
                  iacc, oacc, {scale, 0 /* TODO: Asymm */});
            }
          });
    } else {
      IPEX_DISPATCH_FLOATING_TYPES_AND2(
          at::ScalarType::BFloat16,
          at::ScalarType::Half,
          input_.scalar_type(),
          "aten::adaptive_avg_pool2d",
          [&]() {
            using accscalar_t = at::opmath_type<scalar_t>;
            auto iacc = input_.packed_accessor64<scalar_t, 4>();
            auto oacc = output.packed_accessor64<scalar_t, 4>();
            if (is_smf_channels_last(output)) {
              adaptive_avg_pool2d_kernel<scalar_t, accscalar_t, true>(
                  iacc, oacc);
            } else {
              adaptive_avg_pool2d_kernel<scalar_t, accscalar_t, false>(
                  iacc, oacc);
            }
          });
    }

    if (is_3d) {
      input_.resize_({nInputPlane, inputHeight, inputWidth});
      output.resize_({nInputPlane, outputHeight, outputWidth});
    }
  }
}

} // namespace

namespace at {
namespace AtenIpexTypeQuantizedXPU {

Tensor _adaptive_avg_pool2d(const Tensor& self, IntArrayRef output_size) {
  Tensor output;
  output = at::_empty_affine_quantized(
      {0},
      self.options(),
      self.q_scale(),
      self.q_zero_point(),
      MemoryFormat::Contiguous);
  adaptive_avg_pool2d_out_template(output, self, output_size);
  return output;
}

} // namespace AtenIpexTypeQuantizedXPU
} // namespace at

namespace {
at::Tensor wrapper_QuantizedXPU___adaptive_avg_pool2d(
    const at::Tensor& self,
    c10::SymIntArrayRef output_size) {
  std::optional<Device> common_device = std::nullopt;
  (void)common_device; // Suppress unused variable warning
  c10::impl::check_and_update_common_device(
      common_device,
      self,
      "wrapper_QuantizedXPU___adaptive_avg_pool2d",
      "self");
  const OptionalDeviceGuard device_guard(device_of(self));
  return at::AtenIpexTypeQuantizedXPU::_adaptive_avg_pool2d(
      self, C10_AS_INTARRAYREF_SLOW(output_size));
}

// TORCH_LIBRARY_IMPL(aten, QuantizedXPU, m) {
IPEX_TORCH_LIBRARY_IMPL(aten, QuantizedXPU, m) {
  m.impl(
      "_adaptive_avg_pool2d",
      TORCH_FN(wrapper_QuantizedXPU___adaptive_avg_pool2d));
}
} // anonymous namespace
