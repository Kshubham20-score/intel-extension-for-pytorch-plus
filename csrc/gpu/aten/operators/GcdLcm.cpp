#include <ATen/Context.h>
#include <ATen/native/BinaryOps.h>
#include <ATen/native/TensorIterator.h>

#include <oneDNN/oneDNN.h>
#include <utils/DPCPP.h>
#include "comm/RegistrationDeclarations.h"

#include "Loops.h"
#include "comm/Math.h"
#include "comm/Numerics.h"

using namespace torch_ipex::xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

template <typename scalar_t>
struct LcmKernelDpcppFunctor {
  scalar_t operator()(scalar_t a, scalar_t b) const {
    scalar_t g = calc_gcd(a, b);
    return (g == 0) ? 0 : Numerics<scalar_t>::abs(a / g * b);
  }
};

static void lcm_kernel_dpcpp(TensorIterator& iter) {
  IPEX_DISPATCH_INTEGRAL_TYPES(iter.common_dtype(), "lcm", [&]() {
    LcmKernelDpcppFunctor<scalar_t> kfn;
    dpcpp_fast_mode_kernel_with_scalars(iter, kfn);
  });
}

} // namespace impl

at::Tensor lcm(const Tensor& self, const Tensor& other) {
  Tensor out;
  auto iter = TensorIterator::binary_op(out, self, other);
  impl::lcm_kernel_dpcpp(iter);
  return iter.output();
}

at::Tensor& lcm_out(const Tensor& self, const Tensor& other, Tensor& out) {
  auto iter = TensorIterator::binary_op(out, self, other);
  impl::lcm_kernel_dpcpp(iter);
  return out;
}

at::Tensor& lcm_(Tensor& self, const Tensor& other) {
  return at::AtenIpexTypeXPU::lcm_out(self, other, self);
}

} // namespace AtenIpexTypeXPU
} // namespace at
