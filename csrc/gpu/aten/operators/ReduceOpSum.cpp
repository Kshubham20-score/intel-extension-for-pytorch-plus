#include <ATen/Context.h>
#include <ATen/WrapDimUtils.h>
#include <ATen/WrapDimUtilsMulti.h>
#include <ATen/core/DimVector.h>
#include <ATen/native/ReduceOps.h>
#include <ATen/native/ReduceOpsUtils.h>
#include <ATen/native/TensorIterator.h>

#include <c10/core/ScalarType.h>
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/Numerics.h"

#ifdef USE_OVERRIDE_OP
#include <ATen/DeviceGuard.h>
#include <ATen/core/op_registration/adaption.h>
#include <utils/CustomOperatorRegistration.h>
#endif

#include "Reduce.h"
#include "ReduceOpsUtils.h"

using namespace torch_ipex::xpu::dpcpp;
using namespace at::native;

namespace at {
namespace AtenIpexTypeXPU {

template <typename acc_t, typename data_t>
struct NanSumOps {
  inline acc_t reduce(acc_t a, data_t b, int64_t /*idx*/) const {
    return a + (at::_isnan(b) ? acc_t{0.} : acc_t{b});
  }

  inline acc_t combine(acc_t a, acc_t b) const {
    return a + b;
  }

  inline data_t project(acc_t a) const {
    return data_t{a};
  }

  static acc_t translate_idx(acc_t acc, int64_t /*base_idx*/) {
    return acc;
  }
};

template <typename acc_t>
struct ReduceAddOps {
  ReduceAddOps() {}
  acc_t operator()(acc_t a, acc_t b) const {
    return a + b;
  }
};

template <
    typename scalar_t,
    typename acc_t = scalar_t,
    typename out_t = scalar_t>
void sum_kernel_impl(TensorIterator& iter) {
  dpcpp_reduce_kernel<scalar_t, out_t>(
      iter, func_wrapper<out_t>(ReduceAddOps<acc_t>()));
}

void sum_kernel(TensorIterator& iter) {
  IPEX_DISPATCH_ALL_TYPES_AND_COMPLEX_AND4(
      at::ScalarType::Bool,
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      at::ScalarType::ComplexHalf,
      iter.dtype(),
      "sum",
      [&]() {
        using accscalar_t = at::opmath_type<scalar_t>;
        sum_kernel_impl<scalar_t, accscalar_t>(iter);
      });
}

Tensor& sum_out(
    const Tensor& self,
    OptionalIntArrayRef dim,
    bool keepdim,
    c10::optional<at::ScalarType> opt_dtype,
    Tensor& result) {
  auto out_dtype = infer_dtype_from_optional(self, opt_dtype, result);
  result = resize_reduction(result, self, dim, keepdim, out_dtype);
  auto iter = at::meta::make_reduction_from_out_ty(
      self, result, dim, keepdim, result.scalar_type());
  if (iter.numel() == 0) {
    result.zero_();
  } else {
    sum_kernel(iter);
  }
  return result;
}

Tensor sum(
    const Tensor& self,
    OptionalIntArrayRef dim,
    bool keepdim,
    c10::optional<ScalarType> opt_dtype) {
  ScalarType dtype = get_dtype_from_self(self, opt_dtype, true);
  Tensor result = create_reduction_result(
      self, dim.value_or(IntArrayRef{}), keepdim, dtype);
  return at::AtenIpexTypeXPU::sum_out(self, dim, keepdim, dtype, result);
}

template <
    typename scalar_t,
    typename acc_t = scalar_t,
    typename out_t = scalar_t>
void nansum_kernel_impl(TensorIterator& iter) {
  dpcpp_reduce_kernel<scalar_t, out_t>(iter, NanSumOps<acc_t, out_t>{});
}

Tensor& nansum_out(
    const Tensor& self,
    c10::OptionalArrayRef<int64_t> opt_dim,
    bool keepdim,
    optional<ScalarType> opt_dtype,
    Tensor& result) {
  // For integral types, use existing sum as
  // integral types don't have `Nan`.
  if (c10::isIntegralType(self.scalar_type(), true)) {
    return at::sum_out(result, self, opt_dim, keepdim, opt_dtype);
  }

  auto out_dtype = infer_dtype_from_optional(self, opt_dtype, result);
  result = resize_reduction(result, self, opt_dim, keepdim, out_dtype);
  auto iter = at::meta::make_reduction_from_out_ty(
      self, result, opt_dim, keepdim, result.scalar_type());
  if (iter.numel() == 0) {
    result = result.zero_();
  } else {
    IPEX_DISPATCH_ALL_TYPES_AND_COMPLEX_AND3(
        at::ScalarType::Half,
        at::ScalarType::BFloat16,
        at::ScalarType::ComplexHalf,
        iter.dtype(),
        "nansum",
        [&]() {
          using accscalar_t = at::opmath_type<scalar_t>;
          nansum_kernel_impl<scalar_t, accscalar_t>(iter);
        });
  }
  return result;
}

Tensor nansum(
    const Tensor& self,
    c10::OptionalArrayRef<int64_t> opt_dim,
    bool keepdim,
    c10::optional<ScalarType> opt_dtype) {
  ScalarType dtype = get_dtype_from_self(self, opt_dtype, true);
  Tensor result = create_reduction_result(self, opt_dim, keepdim, dtype);
  return at::AtenIpexTypeXPU::nansum_out(self, opt_dim, keepdim, dtype, result);
}

Tensor nansum(const Tensor& self, c10::optional<ScalarType> dtype) {
  return at::AtenIpexTypeXPU::nansum(
      self, OptionalIntArrayRef{IntArrayRef{}}, false, dtype);
}

} // namespace AtenIpexTypeXPU
} // namespace at

namespace {
#ifdef USE_OVERRIDE_OP
at::Tensor wrapper_XPU_dim_IntList_sum(
    const at::Tensor& self,
    at::OptionalIntArrayRef dim,
    bool keepdim,
    c10::optional<at::ScalarType> dtype) {
  // No device check
  const OptionalDeviceGuard device_guard(device_of(self));

  return at::AtenIpexTypeXPU::sum(self, dim, keepdim, dtype);
}

at::Tensor& wrapper_XPU_IntList_out_sum_out(
    const at::Tensor& self,
    at::OptionalIntArrayRef dim,
    bool keepdim,
    c10::optional<at::ScalarType> dtype,
    at::Tensor& out) {
  // No device check
  const OptionalDeviceGuard device_guard(device_of(self));

  return at::AtenIpexTypeXPU::sum_out(self, dim, keepdim, dtype, out);
}

at::Tensor wrapper_XPU__nansum(
    const at::Tensor& self,
    at::OptionalIntArrayRef dim,
    bool keepdim,
    c10::optional<at::ScalarType> dtype) {
  c10::optional<Device> common_device = nullopt;
  (void)common_device; // Suppress unused variable warning
  c10::impl::check_and_update_common_device(
      common_device, self, "wrapper_XPU__nansum", "self");
  const OptionalDeviceGuard device_guard(device_of(self));

  return at::AtenIpexTypeXPU::nansum(self, dim, keepdim, dtype);
}

at::Tensor& wrapper_XPU_out_nansum_out(
    const at::Tensor& self,
    at::OptionalIntArrayRef dim,
    bool keepdim,
    c10::optional<at::ScalarType> dtype,
    at::Tensor& out) {
  c10::optional<Device> common_device = nullopt;
  (void)common_device; // Suppress unused variable warning
  c10::impl::check_and_update_common_device(
      common_device, out, "wrapper_XPU_out_nansum_out", "out");
  c10::impl::check_and_update_common_device(
      common_device, self, "wrapper_XPU_out_nansum_out", "self");
  const OptionalDeviceGuard device_guard(device_of(self));

  return at::AtenIpexTypeXPU::nansum_out(self, dim, keepdim, dtype, out);
}

IPEX_TORCH_LIBRARY_IMPL(aten, XPU, m) {
  m.impl("sum.dim_IntList", TORCH_FN((&wrapper_XPU_dim_IntList_sum)));
  m.impl("sum.IntList_out", TORCH_FN((&wrapper_XPU_IntList_out_sum_out)));
  m.impl("nansum", TORCH_FN((&wrapper_XPU__nansum)));
  m.impl("nansum.out", TORCH_FN((&wrapper_XPU_out_nansum_out)));
}
#endif
} // namespace
