#include <ATen/ATen.h>
#include <ATen/OpMathType.h>

#include <core/Memory.h>
#include <runtime/Utils.h>
#include <utils/DPCPP.h>
#include <utils/Helpers.h>
#include "PSTLFunctions.h"
#include "Scan.h"
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/MathReduce.h"
#include "comm/Numerics.h"
#include "comm/RegistrationDeclarations.h"

using namespace at::detail;
using namespace torch_ipex::xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {

static c10::MaybeOwned<Tensor> contiguous_out_arg(const Tensor& tensor) {
  if (tensor.is_contiguous()) {
    return c10::MaybeOwned<Tensor>::borrowed(tensor);
  }
  return c10::MaybeOwned<Tensor>::owned(
      at::empty(tensor.sizes(), tensor.options()));
}

void _cummin_helper(
    const Tensor& self,
    Tensor& values,
    Tensor& indices,
    int64_t dim) {
  TORCH_CHECK(
      values.device() == indices.device() && values.device() == self.device(),
      "Expected tensors for cummin have the same device",
      "; but devices are ",
      self.device(),
      " , ",
      values.device(),
      " , ",
      indices.device());
  auto values_ = contiguous_out_arg(values);
  auto indices_ = contiguous_out_arg(indices);
  IPEX_DISPATCH_ALL_TYPES_AND3(
      at::ScalarType::Bool,
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      self.scalar_type(),
      "cummin",
      [&]() {
        scalar_t init = self.is_floating_point()
            ? std::numeric_limits<scalar_t>::infinity()
            : std::numeric_limits<scalar_t>::max();
        scan_with_indices<INCLUSIVE_TYPE, scalar_t, scalar_t>(
            self, values, indices, dim, init, LessEqOp<scalar_t>());
      });

  if (!values.is_same(*values_)) {
    values.copy_(*values_);
  }
  if (!indices.is_same(*indices_)) {
    indices.copy_(*indices_);
  }
}

void _cummax_helper(
    const Tensor& self,
    Tensor& values,
    Tensor& indices,
    int64_t dim) {
  TORCH_CHECK(
      values.device() == indices.device() && values.device() == self.device(),
      "Expected tensors for cummax have the same device",
      "; but devices are ",
      self.device(),
      " , ",
      values.device(),
      " , ",
      indices.device());
  auto values_ = contiguous_out_arg(values);
  auto indices_ = contiguous_out_arg(indices);
  IPEX_DISPATCH_ALL_TYPES_AND3(
      at::ScalarType::Bool,
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      self.scalar_type(),
      "cummax",
      [&]() {
        scalar_t init = self.is_floating_point()
            ? -std::numeric_limits<scalar_t>::infinity()
            : std::numeric_limits<scalar_t>::lowest();
        scan_with_indices<INCLUSIVE_TYPE, scalar_t, scalar_t>(
            self, values, indices, dim, init, GreaterEqOp<scalar_t>());
      });

  if (!values.is_same(*values_)) {
    values.copy_(*values_);
  }
  if (!indices.is_same(*indices_)) {
    indices.copy_(*indices_);
  }
}

Tensor _logcumsumexp(const Tensor& self, int64_t dim) {
  Tensor result = at::empty_like(self, at::MemoryFormat::Contiguous);
  return _logcumsumexp_out(self, dim, result);
}

template <typename scalar_t, typename opmath_t>
struct _logcumsumexp_out_log_add_exp_functor {
  scalar_t operator()(const scalar_t x_, const scalar_t y_) const {
    const opmath_t x{x_}, y{y_};

    auto isnan_x = at::_isnan(x);
    auto isnan_y = at::_isnan(y);
    opmath_t min = isnan_y ? y : (isnan_x ? x : std::min(x, y));
    opmath_t max = isnan_y ? y : (isnan_x ? x : std::max(x, y));
    if (min != max || ::isfinite(min)) {
      // nan will be propagated here
      return ::log1p(std::exp(min - max)) + max;
    } else {
      // special case to correctly handle infinite cases
      return x;
    }
  }
};

Tensor& _logcumsumexp_out(const Tensor& self, int64_t dim, Tensor& result) {
  const auto wrap_dim = maybe_wrap_dim(dim, self.dim());
  result.resize_(self.sizes());
  if (self.dim() == 0) {
    result.fill_(self);
    return result;
  }
  if (self.numel() == 0) {
    result.zero_();
    return result;
  }
  auto result_ = contiguous_out_arg(result);
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      self.scalar_type(),
      "logcumsumexp_out_dpcpp",
      [&]() {
        using opmath_t = at::opmath_type<scalar_t>;
        scalar_t init = Numerics<scalar_t>::lower_bound();
        _logcumsumexp_out_log_add_exp_functor<scalar_t, opmath_t> log_add_exp;
        scan<INCLUSIVE_TYPE, scalar_t, scalar_t>(
            *result_, self, wrap_dim, init, log_add_exp);
      });
  if (!result.is_same(*result_)) {
    result.copy_(*result_);
  }
  return result;
}

} // namespace AtenIpexTypeXPU
} // namespace at
