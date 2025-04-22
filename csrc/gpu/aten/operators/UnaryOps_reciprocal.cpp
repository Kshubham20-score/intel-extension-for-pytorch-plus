#include <ATen/ATen.h>
#include <ATen/native/TensorIterator.h>

#include <utils/DPCPP.h>
#include "Loops.h"
#include "comm/ATDispatch.h"
#include "comm/Numerics.h"

using namespace torch_ipex::xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {

template <typename T>
static inline T reciprocal_wrapper(T a) {
  return static_cast<T>(1) / a;
}

template <typename T>
static inline c10::complex<T> reciprocal_wrapper(c10::complex<T> v) {
  // Handle extreme cases for numpy compatibility
  auto both_inf = [](T real, T imag) {
    return (Numerics<T>::isinf(real) && Numerics<T>::isinf(imag));
  };

  auto either_inf = [](T real, T imag) {
    return Numerics<T>::isinf(real) || Numerics<T>::isinf(imag);
  };

  auto either_nan = [](T real, T imag) {
    return Numerics<T>::isnan(real) || Numerics<T>::isnan(imag);
  };

  if (either_nan(v.real(), v.imag()) || both_inf(v.real(), v.imag())) {
    // If either is Nan or both are infinite, return {nan, nan}
    return {
        std::numeric_limits<T>::quiet_NaN(),
        std::numeric_limits<T>::quiet_NaN()};
  } else if (either_inf(v.real(), v.imag())) {
    // If either is Inf, return {0, 0}
    return {0, 0};
  }
  const c10::complex<T> one = c10::complex<T>(1.0, 0);
  return one / v;
}

template <typename scalar_t>
struct reciprocal_kernel_xpu_functor {
  scalar_t operator()(scalar_t a) const {
    return reciprocal_wrapper(a);
  }
};

void reciprocal_kernel_xpu(TensorIterator& iter) {
  IPEX_DISPATCH_FLOATING_AND_COMPLEX_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "reciprocal_xpu",
      [&] {
        reciprocal_kernel_xpu_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
}

} // namespace AtenIpexTypeXPU
} // namespace at
