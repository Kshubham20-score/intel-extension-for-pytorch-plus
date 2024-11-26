#include <ATen/Context.h>
#include <ATen/OpMathType.h>
#include <ATen/native/BinaryOps.h>
#include <ATen/native/TensorIterator.h>

#include <oneDNN/oneDNN.h>
#include <utils/DPCPP.h>
#include "comm/RegistrationDeclarations.h"
#include "comm/ScalarOps.h"

#include "Loops.h"
#include "LoopsTemplates.h"
#include "comm/zmath.h"

using namespace torch_ipex::xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

template <typename scalar_t>
struct mul_kernel_dpcpp_functor {
  scalar_t operator()(scalar_t a, scalar_t b) const {
    return a * b;
  }
};

static void mul_kernel_dpcpp(TensorIteratorBase& iter) {
  IPEX_DISPATCH_ALL_TYPES_AND_COMPLEX_AND4(
      at::ScalarType::BFloat16,
      at::ScalarType::Half,
      at::ScalarType::Bool,
      at::ScalarType::ComplexHalf,
      iter.dtype(),
      "mul",
      [&]() {
        using opmath_t = at::opmath_type<scalar_t>;
        mul_kernel_dpcpp_functor<opmath_t> f;
        fast_mode_opmath_symmetric_gpu_kernel_with_scalars<scalar_t>(iter, f);
      });
}
} // namespace impl

Tensor& mul_out(const Tensor& self, const Tensor& other, Tensor& result) {
  return binary_out_template<dnnl::algorithm::binary_mul>(
      TensorIterator::binary_op,
      result,
      self,
      other,
      [=](TensorIteratorBase& iter) { impl::mul_kernel_dpcpp(iter); });
}

Tensor mul(const Tensor& self, const Tensor& other) {
  Tensor result;
  return binary_out_template<dnnl::algorithm::binary_mul>(
      TensorIterator::binary_op,
      result,
      self,
      other,
      [=](TensorIteratorBase& iter) { impl::mul_kernel_dpcpp(iter); });
}

Tensor mul(const Tensor& self, const Scalar& other) {
  return at::AtenIpexTypeXPU::mul(self, wrapped_scalar_tensor(other));
}

} // namespace AtenIpexTypeXPU
} // namespace at
