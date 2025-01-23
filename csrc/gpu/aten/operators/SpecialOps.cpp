#include <ATen/ATen.h>
#include <ATen/OpMathType.h>
#include <ATen/native/BinaryOps.h>
#include <ATen/native/TensorIterator.h>
#include <core/Memory.h>
#include <runtime/Utils.h>
#include <utils/oneMKLUtils.h>
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/Math.h"
#include "comm/Numerics.h"
#include "comm/RegistrationDeclarations.h"

#include "Loops.h"

#include <ATen/Context.h>
#ifdef USE_OVERRIDE_OP
#include <ATen/DeviceGuard.h>
#include <ATen/core/op_registration/adaption.h>
#include <utils/CustomOperatorRegistration.h>
#endif
#include <utils/DPCPP.h>
#include "comm/Numerics.h"
#include "comm/Pointwise.h"
#include "comm/ScalarOps.h"

using namespace torch_ipex::xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {

template <typename scalar_t, typename accscalar_t>
struct i0_out_functor {
  scalar_t operator()(scalar_t a) const {
    accscalar_t x = static_cast<accscalar_t>(a);
    return (scalar_t)(calc_i0(x));
  }
};

Tensor& i0_out(const Tensor& self, Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "i0_out",
      [&]() {
        using accscalar_t = at::opmath_type<scalar_t>;
        i0_out_functor<scalar_t, accscalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_ndtri_out_functor {
  scalar_t operator()(scalar_t a) const {
    return calc_ndtri(a);
  }
};

Tensor& special_ndtri_out(const Tensor& self, Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "special_ndtri_out",
      [&]() {
        special_ndtri_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t, typename accscalar_t>
struct special_i0e_out_functor {
  scalar_t operator()(scalar_t a) const {
    accscalar_t x = static_cast<accscalar_t>(a);
    return (scalar_t)(calc_i0e(x));
  }
};

Tensor& special_i0e_out(const Tensor& self, Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "i0e",
      [&]() {
        using accscalar_t = at::opmath_type<scalar_t>;
        special_i0e_out_functor<scalar_t, accscalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t, typename accscalar_t>
struct special_i1_out_functor {
  scalar_t operator()(scalar_t a) const {
    accscalar_t x = static_cast<accscalar_t>(a);
    return (scalar_t)(calc_i1(x));
  }
};

Tensor& special_i1_out(const Tensor& self, Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "i1", [&]() {
        using accscalar_t = acc_type<scalar_t>;
        special_i1_out_functor<scalar_t, accscalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t, typename accscalar_t>
struct special_i1e_out_functor {
  scalar_t operator()(scalar_t a) const {
    accscalar_t x = static_cast<accscalar_t>(a);
    return (scalar_t)(calc_i1e(x));
  }
};

Tensor& special_i1e_out(const Tensor& self, Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "i1e", [&]() {
        using accscalar_t = acc_type<scalar_t>;
        special_i1e_out_functor<scalar_t, accscalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_entr_out_functor {
  scalar_t operator()(scalar_t x) const {
    if (at::_isnan(x)) {
      return x;
    } else if (x > 0) {
      return -x * Numerics<scalar_t>::log(x);
    } else if (x == 0) {
      return 0;
    }
    return Numerics<scalar_t>::lower_bound();
  }
};

Tensor& special_entr_out(const Tensor& self, Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      ScalarType::Half,
      ScalarType::BFloat16,
      iter.common_dtype(),
      "entr",
      [&]() {
        special_entr_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_erfcx_out_functor {
  scalar_t operator()(scalar_t a) const {
    return calc_erfcx(a);
  }
};

Tensor& special_erfcx_out(const Tensor& self, Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "erfcx", [&]() {
        special_erfcx_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_xlog1py_OutFunctor {
  scalar_t operator()(scalar_t x, scalar_t y) const {
    if (at::_isnan(y)) {
      return NAN;
    }
    if (x == 0) {
      return 0;
    }
    return x * Numerics<scalar_t>::log1p(y);
  }
};

Tensor& special_xlog1py_out(
    const Tensor& self,
    const Tensor& other,
    at::Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "xlog1py",
      [&]() {
        special_xlog1py_OutFunctor<scalar_t> f;
        dpcpp_kernel_with_scalars(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_bessel_j0_out_functor {
  scalar_t operator()(scalar_t a) const {
    return bessel_j0_forward(a);
  }
};

Tensor& special_bessel_j0_out(const Tensor& self, at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "bessel_j0", [&]() {
        special_bessel_j0_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_bessel_j1_out_functor {
  scalar_t operator()(scalar_t a) const {
    if (a < scalar_t(0.0f)) {
      return -bessel_j1_forward(-a);
    }
    return bessel_j1_forward(a);
  }
};

Tensor& special_bessel_j1_out(const Tensor& self, at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "bessel_j1", [&]() {
        special_bessel_j1_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_bessel_y0_out_functor {
  scalar_t operator()(scalar_t a) const {
    return bessel_y0_forward(a);
  }
};

Tensor& special_bessel_y0_out(const Tensor& self, at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "bessel_y0", [&]() {
        special_bessel_y0_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_bessel_y1_out_functor {
  scalar_t operator()(scalar_t a) const {
    return bessel_y1_forward(a);
  }
};

Tensor& special_bessel_y1_out(const Tensor& self, at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "bessel_y1", [&]() {
        special_bessel_y1_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_chebyshev_polynomial_t_out_functor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return chebyshev_polynomial_t_forward<scalar_t>(x, n);
  }
};

Tensor& special_chebyshev_polynomial_t_out(
    const Tensor& self,
    const Tensor& other,
    at::Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "chebyshev_polynomial_t",
      [&]() {
        special_chebyshev_polynomial_t_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_chebyshev_polynomial_u_out_functor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return chebyshev_polynomial_u_forward<scalar_t>(x, n);
  }
};

Tensor& special_chebyshev_polynomial_u_out(
    const Tensor& self,
    const Tensor& other,
    at::Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "chebyshev_polynomial_u",
      [&]() {
        special_chebyshev_polynomial_u_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_chebyshev_polynomial_v_out_functor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return chebyshev_polynomial_v_forward<scalar_t>(x, n);
  }
};

Tensor& special_chebyshev_polynomial_v_out(
    const Tensor& self,
    const Tensor& other,
    at::Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "chebyshev_polynomial_v",
      [&]() {
        special_chebyshev_polynomial_v_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_chebyshev_polynomial_w_out_functor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return chebyshev_polynomial_w_forward<scalar_t>(x, n);
  }
};

Tensor& special_chebyshev_polynomial_w_out(
    const Tensor& self,
    const Tensor& other,
    at::Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "chebyshev_polynomial_w",
      [&]() {
        special_chebyshev_polynomial_w_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct SpecialZetaOutFunctor {
  scalar_t operator()(scalar_t x, scalar_t q) const {
    return zeta<scalar_t>(x, q);
  }
};

Tensor& special_zeta_out(const Tensor& self, const Tensor& other, Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "zeta", [&]() {
        SpecialZetaOutFunctor<scalar_t> f;
        dpcpp_kernel_with_scalars(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_spherical_bessel_j0_out_functor {
  scalar_t operator()(scalar_t a) const {
    return spherical_bessel_j0_forward(a);
  }
};

Tensor& special_spherical_bessel_j0_out(const Tensor& self, at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "spherical_bessel_j0",
      [&]() {
        special_spherical_bessel_j0_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct SpecialHermitePolynomialHeOutFunctor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return hermite_polynomial_he_forward<scalar_t>(x, n);
  }
};

Tensor& special_hermite_polynomial_he_out(
    const Tensor& self,
    const Tensor& other,
    Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "hermite_polynomial_he",
      [&]() {
        SpecialHermitePolynomialHeOutFunctor<scalar_t> f;
        dpcpp_kernel_with_scalars(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct SpecialHermitePolynomialHOutFunctor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return hermite_polynomial_h_forward<scalar_t>(x, n);
  }
};

Tensor& special_hermite_polynomial_h_out(
    const Tensor& self,
    const Tensor& other,
    Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "", [&]() {
        SpecialHermitePolynomialHOutFunctor<scalar_t> f;
        dpcpp_kernel_with_scalars(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct SpecialHermitePolynomialLOutFunctor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return laguerre_polynomial_l_forward<scalar_t>(x, n);
  }
};

Tensor& special_laguerre_polynomial_l_out(
    const Tensor& self,
    const Tensor& other,
    Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "laguerre_polynomial_l",
      [&]() {
        SpecialHermitePolynomialLOutFunctor<scalar_t> f;
        dpcpp_kernel_with_scalars(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct SpecialHermitePolynomialPOutFunctor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return legendre_polynomial_p_forward<scalar_t>(x, n);
  }
};

Tensor& special_legendre_polynomial_p_out(
    const Tensor& self,
    const Tensor& other,
    Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "legendre_polynomial_p",
      [&]() {
        SpecialHermitePolynomialPOutFunctor<scalar_t> f;
        dpcpp_kernel_with_scalars(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_modified_bessel_i0_out_functor {
  scalar_t operator()(scalar_t a) const {
    return modified_bessel_i0_forward(a);
  }
};

Tensor& special_modified_bessel_i0_out(const Tensor& self, at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "modified_bessel_i0",
      [&]() {
        special_modified_bessel_i0_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_modified_bessel_i1_out_functor {
  scalar_t operator()(scalar_t a) const {
    return modified_bessel_i1_forward(a);
  }
};

Tensor& special_modified_bessel_i1_out(const Tensor& self, at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "modified_bessel_i1",
      [&]() {
        special_modified_bessel_i1_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_modified_bessel_k0_out_functor {
  scalar_t operator()(scalar_t a) const {
    return modified_bessel_k0_forward(a);
  }
};

Tensor& special_modified_bessel_k0_out(const Tensor& self, at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "modified_bessel_k0",
      [&]() {
        special_modified_bessel_k0_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_modified_bessel_k1_out_functor {
  scalar_t operator()(scalar_t a) const {
    return modified_bessel_k1_forward(a);
  }
};

Tensor& special_modified_bessel_k1_out(const Tensor& self, at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "modified_bessel_k1",
      [&]() {
        special_modified_bessel_k1_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_scaled_modified_bessel_k0_out_functor {
  scalar_t operator()(scalar_t a) const {
    return scaled_modified_bessel_k0_forward(a);
  }
};

Tensor& special_scaled_modified_bessel_k0_out(
    const Tensor& self,
    at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "scaled_modified_bessel_k0",
      [&]() {
        special_scaled_modified_bessel_k0_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_scaled_modified_bessel_k1_out_functor {
  scalar_t operator()(scalar_t a) const {
    return scaled_modified_bessel_k1_forward(a);
  }
};

Tensor& special_scaled_modified_bessel_k1_out(
    const Tensor& self,
    at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      iter.common_dtype(),
      "scaled_modified_bessel_k1",
      [&]() {
        special_scaled_modified_bessel_k1_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_log_ndtr_out_functor {
  scalar_t operator()(scalar_t a) const {
    return calc_log_ndtr(a);
  }
};

Tensor& special_log_ndtr_out(const Tensor& self, at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "log_ndtr", [&]() {
        special_log_ndtr_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_shifted_chebyshev_polynomial_t_out_functor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return shifted_chebyshev_polynomial_t_forward<scalar_t>(x, n);
  }
};

Tensor& special_shifted_chebyshev_polynomial_t_out(
    const Tensor& self,
    const Tensor& other,
    at::Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES(
      iter.common_dtype(), "shifted_chebyshev_polynomial_t", [&]() {
        special_shifted_chebyshev_polynomial_t_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_shifted_chebyshev_polynomial_u_out_functor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return shifted_chebyshev_polynomial_u_forward<scalar_t>(x, n);
  }
};

Tensor& special_shifted_chebyshev_polynomial_u_out(
    const Tensor& self,
    const Tensor& other,
    at::Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES(
      iter.common_dtype(), "shifted_chebyshev_polynomial_u", [&]() {
        special_shifted_chebyshev_polynomial_u_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_shifted_chebyshev_polynomial_v_out_functor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return shifted_chebyshev_polynomial_v_forward<scalar_t>(x, n);
  }
};

Tensor& special_shifted_chebyshev_polynomial_v_out(
    const Tensor& self,
    const Tensor& other,
    at::Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES(
      iter.common_dtype(), "shifted_chebyshev_polynomial_v", [&]() {
        special_shifted_chebyshev_polynomial_v_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_shifted_chebyshev_polynomial_w_out_functor {
  scalar_t operator()(scalar_t x, scalar_t n) const {
    return shifted_chebyshev_polynomial_w_forward<scalar_t>(x, n);
  }
};

Tensor& special_shifted_chebyshev_polynomial_w_out(
    const Tensor& self,
    const Tensor& other,
    at::Tensor& out) {
  auto iter = TensorIterator::binary_float_op(out, self, other);
  IPEX_DISPATCH_FLOATING_TYPES(
      iter.common_dtype(), "shifted_chebyshev_polynomial_w", [&]() {
        special_shifted_chebyshev_polynomial_w_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct special_airy_ai_out_functor {
  scalar_t operator()(scalar_t x) const {
    return airy_ai_forward(x);
  }
};

Tensor& special_airy_ai_out(const Tensor& self, at::Tensor& out) {
  auto iter = TensorIterator::unary_float_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES(iter.common_dtype(), "airy_ai", [&]() {
    special_airy_ai_out_functor<scalar_t> f;
    dpcpp_kernel_for_tensor_iter(iter, f);
  });
  return out;
}

#ifdef USE_OVERRIDE_OP
at::Tensor special_erfcx(const at::Tensor& self) {
  Tensor out;
  TensorIterator iter;
  iter.build_borrowing_unary_float_op(out, self);

  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "erfcx", [&]() {
        special_erfcx_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return iter.output();
}

at::Tensor special_ndtri(const at::Tensor& self) {
  Tensor out;
  TensorIterator iter;
  iter.build_borrowing_unary_float_op(out, self);

  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16, iter.common_dtype(), "special_ndtri", [&]() {
        special_ndtri_out_functor<scalar_t> f;
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return iter.output();
}
#endif
} // namespace AtenIpexTypeXPU
} // namespace at
#ifdef USE_OVERRIDE_OP
at::Tensor wrapper_XPU_special_erfcx(const at::Tensor& self) {
  std::optional<Device> common_device = std::nullopt;
  (void)common_device; // Suppress unused variable warning
  c10::impl::check_and_update_common_device(
      common_device, self, "wrapper_XPU_special_erfcx", "self");
  const OptionalDeviceGuard device_guard(device_of(self));

  return at::AtenIpexTypeXPU::special_erfcx(self);
}

at::Tensor wrapper_XPU_special_ndtri(const at::Tensor& self) {
  std::optional<Device> common_device = std::nullopt;
  (void)common_device; // Suppress unused variable warning
  c10::impl::check_and_update_common_device(
      common_device, self, "wrapper_XPU_special_ndtri", "self");
  const OptionalDeviceGuard device_guard(device_of(self));

  return at::AtenIpexTypeXPU::special_ndtri(self);
}

namespace {
IPEX_TORCH_LIBRARY_IMPL(aten, XPU, m) {
  m.impl("special_erfcx", TORCH_FN((&wrapper_XPU_special_erfcx)));
  m.impl("special_ndtri", TORCH_FN((&wrapper_XPU_special_ndtri)));
}
} // namespace
#endif
