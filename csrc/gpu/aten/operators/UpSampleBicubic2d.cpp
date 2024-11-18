#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/core/TensorAccessor.h>
#include <ATen/native/UpSample.h>

#include <core/Memory.h>
#include <utils/DPCPP.h>
#include <utils/Helpers.h>
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/RegistrationDeclarations.h"

#include "UpSample.h"
#include "comm/Numerics.h"

using namespace torch_ipex::xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

template <typename scalar_t, typename accscalar_t>
struct UpsampleBicubic2dBackwardOutFrameKernelFunctor {
  void operator()(sycl::nd_item<1> item) const {
    auto idata = in_data;
    auto odata = out_data;
    int global_id = item.get_global_linear_id();
    const int nbatch = idata.size(0);
    const int channels = idata.size(1);
    const int input_height = idata.size(2);
    const int input_width = idata.size(3);
    const int output_height = odata.size(2);
    const int output_width = odata.size(3);

    if (global_id < output_height * output_width) {
      const int output_x = global_id % output_width;
      const int output_y = global_id / output_width;
      // special case: output_xust copy
      if (input_height == output_height && input_width == output_width) {
        for (int n = 0; n < nbatch; n++) {
          for (int c = 0; c < channels; ++c) {
            auto val = odata[n][c][output_y][output_x];
            idata[n][c][output_y][output_x] = val;
          }
        }
        return;
      }

      accscalar_t real_x = area_pixel_compute_source_index(
          width_scale, output_x, align_corners, /*cubic=*/true);
      int input_x = Numerics<accscalar_t>::floor(real_x);
      accscalar_t t_x = real_x - input_x;

      accscalar_t real_y = area_pixel_compute_source_index(
          height_scale, output_y, align_corners, /*cubic=*/true);
      int input_y = Numerics<accscalar_t>::floor(real_y);
      accscalar_t t_y = real_y - input_y;

      accscalar_t x_coeffs[4];
      accscalar_t y_coeffs[4];

      get_cubic_upsample_coefficients(x_coeffs, t_x);
      get_cubic_upsample_coefficients(y_coeffs, t_y);

      for (int n = 0; n < nbatch; n++) {
        for (int c = 0; c < channels; ++c) {
          scalar_t out_value = odata[n][c][output_y][output_x];
          for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
              upsample_increment_value_bounded<scalar_t>(
                  idata,
                  n,
                  c,
                  input_width,
                  input_height,
                  input_x - 1 + j,
                  input_y - 1 + i,
                  out_value * y_coeffs[i] * x_coeffs[j]);
            }
          }
        }
      }
    }
  }
  UpsampleBicubic2dBackwardOutFrameKernelFunctor(
      const PackedTensorAccessor64<scalar_t, 4> out_data_,
      PackedTensorAccessor64<scalar_t, 4> in_data_,
      int64_t onum_,
      bool align_corners_,
      const accscalar_t height_scale_,
      const accscalar_t width_scale_)
      : out_data(out_data_),
        in_data(in_data_),
        onum(onum_),
        align_corners(align_corners_),
        height_scale(height_scale_),
        width_scale(width_scale_) {}

 private:
  const PackedTensorAccessor64<scalar_t, 4> out_data;
  PackedTensorAccessor64<scalar_t, 4> in_data;
  int64_t onum;
  bool align_corners;
  const accscalar_t height_scale;
  const accscalar_t width_scale;
};

// Backward (adjoint) operation 1 <- 2 (accumulates)
template <typename scalar_t, typename accscalar_t>
static void upsample_bicubic2d_backward_out_frame(
    const PackedTensorAccessor64<scalar_t, 4> odata,
    PackedTensorAccessor64<scalar_t, 4> idata,
    int64_t onum,
    bool align_corners,
    const accscalar_t height_scale,
    const accscalar_t width_scale) {
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  int64_t local_range = static_cast<int64_t>(1);
  int64_t global_range = static_cast<int64_t>(1);
  if (onum != 0) {
    int64_t wg_size = dpcppMaxWorkGroupSize();
    local_range = onum < wg_size ? onum : wg_size;
    global_range = ((onum + local_range - 1) / local_range) * local_range;
  }
  auto cgf = DPCPP_Q_CGF(cgh) {
    auto in_data = idata;
    auto out_data = odata;

    UpsampleBicubic2dBackwardOutFrameKernelFunctor<scalar_t, accscalar_t> kfn(
        out_data, in_data, onum, align_corners, height_scale, width_scale);
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<1>(
            sycl::range<1>(global_range), sycl::range<1>(local_range)),
        kfn);
  };
  DPCPP_Q_SUBMIT(dpcpp_queue, cgf);
}

static void upsample_bicubic2d_backward_out_template(
    Tensor& grad_input,
    const Tensor& grad_output_,
    IntArrayRef output_size,
    IntArrayRef input_size,
    bool align_corners,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  TORCH_CHECK(
      output_size.size() == 2,
      "It is expected output_size equals to 2, but got size ",
      output_size.size());

  TORCH_CHECK(
      input_size.size() == 4,
      "It is expected input_size equals to 4, but got size ",
      input_size.size());

  int64_t output_height = output_size[0];
  int64_t output_width = output_size[1];

  int64_t nbatch = input_size[0];
  int64_t channels = input_size[1];
  int64_t input_height = input_size[2];
  int64_t input_width = input_size[3];

  upsample_2d_shape_check(
      Tensor(),
      grad_output_,
      nbatch,
      channels,
      input_height,
      input_width,
      output_height,
      output_width);

  Tensor grad_output = grad_output_.contiguous();

  grad_input.resize_({nbatch, channels, input_height, input_width});
  grad_input.zero_();

  IPEX_DISPATCH_FLOATING_TYPES_AND_HALF(
      grad_output.scalar_type(), "upsample_bicubic2d_backward", [&] {
        auto idata = grad_input.packed_accessor64<scalar_t, 4>();
        auto odata = grad_output.packed_accessor64<scalar_t, 4>();
        auto onum = grad_output.numel();

        using accscalar_t = at::AtenIpexTypeXPU::acc_type<scalar_t>;
        const accscalar_t rheight = area_pixel_compute_scale<accscalar_t>(
            input_height, output_height, align_corners, scales_h);
        const accscalar_t rwidth = area_pixel_compute_scale<accscalar_t>(
            input_width, output_width, align_corners, scales_w);

        upsample_bicubic2d_backward_out_frame<scalar_t, accscalar_t>(
            odata, idata, onum, align_corners, rheight, rwidth);
      });
}
} // namespace impl

using at::native::upsample::compute_output_size;
using at::native::upsample::get_scale_value;

Tensor& upsample_bicubic2d_backward_out(
    const Tensor& grad_output,
    IntArrayRef output_size,
    IntArrayRef input_size,
    bool align_corners,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w,
    Tensor& grad_input) {
  impl::upsample_bicubic2d_backward_out_template(
      grad_input,
      grad_output,
      output_size,
      input_size,
      align_corners,
      scales_h,
      scales_w);
  return grad_input;
}

Tensor upsample_bicubic2d_backward(
    const Tensor& grad_output,
    IntArrayRef output_size,
    IntArrayRef input_size,
    bool align_corners,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  auto grad_input = at::zeros(input_size, grad_output.options());
  impl::upsample_bicubic2d_backward_out_template(
      grad_input,
      grad_output,
      output_size,
      input_size,
      align_corners,
      scales_h,
      scales_w);
  return grad_input;
}

Tensor upsample_bicubic2d_backward(
    const Tensor& grad_output,
    c10::optional<IntArrayRef> output_size,
    IntArrayRef input_size,
    bool align_corners,
    c10::optional<ArrayRef<double>> scale_factors) {
  auto osize = compute_output_size(input_size, output_size, scale_factors);
  auto scale_h = get_scale_value(scale_factors, 0);
  auto scale_w = get_scale_value(scale_factors, 1);
  auto grad_input = at::zeros(input_size, grad_output.options());
  impl::upsample_bicubic2d_backward_out_template(
      grad_input,
      grad_output,
      osize,
      input_size,
      align_corners,
      scale_h,
      scale_w);
  return grad_input;
}

} // namespace AtenIpexTypeXPU
} // namespace at
