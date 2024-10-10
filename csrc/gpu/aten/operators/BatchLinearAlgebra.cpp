#include <ATen/Context.h>
#include <ATen/NativeFunctions.h>
#include <ATen/native/LinearAlgebraUtils.h>
#include <ATen/native/Resize.h>
#include <ATen/ops/_linalg_check_errors.h>

#include <runtime/Utils.h>
#include <tensor/TensorMeta.h>
#include <utils/oneMKLUtils.h>

#include "comm/ATDispatch.h"
#include "comm/ApplyUtils.h"
#include "comm/Numerics.h"
#include "comm/RegistrationDeclarations.h"

#include "Resize.h"

#include <torch/custom_class.h>
#include "comm/ParamUtils.h"

using namespace torch_ipex::xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {

// Used as an interface between the different BLAS-like libraries
enum class TransposeType {
  NoTranspose,
  Transpose,
  ConjTranspose,
};

#ifdef USE_ONEMKL
// Transforms TransposeType into the BLAS / LAPACK format
static oneapi::mkl::transpose to_blas(TransposeType trans) {
  switch (trans) {
    case TransposeType::Transpose:
      return oneapi::mkl::transpose::trans;
    case TransposeType::NoTranspose:
      return oneapi::mkl::transpose::nontrans;
    case TransposeType::ConjTranspose:
      return oneapi::mkl::transpose::conjtrans;
  }
  TORCH_INTERNAL_ASSERT(false, "Invalid transpose type");
}
#endif

namespace impl {

#ifdef USE_ONEMKL
template <typename scalar_t>
int64_t mkl_getrf_scratchpad(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_ipiv,
    int64_t batch_size) {
  return oneapi::mkl::lapack::getrf_batch_scratchpad_size<scalar_t>(
      queue, m, n, lda, stride_a, stride_ipiv, batch_size);
}

template <>
int64_t mkl_getrf_scratchpad<c10::complex<double>>(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_ipiv,
    int64_t batch_size) {
  return oneapi::mkl::lapack::getrf_batch_scratchpad_size<std::complex<double>>(
      queue, m, n, lda, stride_a, stride_ipiv, batch_size);
}

template <>
int64_t mkl_getrf_scratchpad<c10::complex<float>>(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_ipiv,
    int64_t batch_size) {
  return oneapi::mkl::lapack::getrf_batch_scratchpad_size<std::complex<float>>(
      queue, m, n, lda, stride_a, stride_ipiv, batch_size);
}

template <typename scalar_t>
int64_t mkl_getrs_scratchpad(
    sycl::queue& queue,
    oneapi::mkl::transpose trans,
    int64_t n,
    int64_t nrhs,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_ipiv,
    int64_t ldb,
    int64_t stride_b,
    int64_t batch_size) {
  return oneapi::mkl::lapack::getrs_batch_scratchpad_size<scalar_t>(
      queue,
      trans,
      n,
      nrhs,
      lda,
      stride_a,
      stride_ipiv,
      ldb,
      stride_b,
      batch_size);
}

template <>
int64_t mkl_getrs_scratchpad<c10::complex<double>>(
    sycl::queue& queue,
    oneapi::mkl::transpose trans,
    int64_t n,
    int64_t nrhs,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_ipiv,
    int64_t ldb,
    int64_t stride_b,
    int64_t batch_size) {
  return oneapi::mkl::lapack::getrs_batch_scratchpad_size<std::complex<double>>(
      queue,
      trans,
      n,
      nrhs,
      lda,
      stride_a,
      stride_ipiv,
      ldb,
      stride_b,
      batch_size);
}

template <>
int64_t mkl_getrs_scratchpad<c10::complex<float>>(
    sycl::queue& queue,
    oneapi::mkl::transpose trans,
    int64_t n,
    int64_t nrhs,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_ipiv,
    int64_t ldb,
    int64_t stride_b,
    int64_t batch_size) {
  return oneapi::mkl::lapack::getrs_batch_scratchpad_size<std::complex<float>>(
      queue,
      trans,
      n,
      nrhs,
      lda,
      stride_a,
      stride_ipiv,
      ldb,
      stride_b,
      batch_size);
}

template <typename scalar_t>
int64_t mkl_getri_scratchpad(
    sycl::queue& queue,
    int64_t n,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_ipiv,
    int64_t ldainv,
    int64_t stride_ainv,
    int64_t batch_size) {
  return oneapi::mkl::lapack::getri_batch_scratchpad_size<scalar_t>(
      queue, n, lda, stride_a, stride_ipiv, ldainv, stride_ainv, batch_size);
}

template <>
int64_t mkl_getri_scratchpad<c10::complex<double>>(
    sycl::queue& queue,
    int64_t n,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_ipiv,
    int64_t ldainv,
    int64_t stride_ainv,
    int64_t batch_size) {
  return oneapi::mkl::lapack::getri_batch_scratchpad_size<std::complex<double>>(
      queue, n, lda, stride_a, stride_ipiv, ldainv, stride_ainv, batch_size);
}

template <>
int64_t mkl_getri_scratchpad<c10::complex<float>>(
    sycl::queue& queue,
    int64_t n,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_ipiv,
    int64_t ldainv,
    int64_t stride_ainv,
    int64_t batch_size) {
  return oneapi::mkl::lapack::getri_batch_scratchpad_size<std::complex<float>>(
      queue, n, lda, stride_a, stride_ipiv, ldainv, stride_ainv, batch_size);
}

template <typename scalar_t>
void mkl_getrf(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    scalar_t* a,
    int64_t lda,
    int64_t stride_a,
    int64_t* ipiv,
    int64_t stride_ipiv,
    int64_t batch_size,
    scalar_t* scratchpad,
    int scratchpadsize) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::getrf_batch,
      queue,
      m,
      n,
      a,
      lda,
      stride_a,
      ipiv,
      stride_ipiv,
      batch_size,
      scratchpad,
      scratchpadsize);
}

template <>
void mkl_getrf<c10::complex<double>>(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    c10::complex<double>* a,
    int64_t lda,
    int64_t stride_a,
    int64_t* ipiv,
    int64_t stride_ipiv,
    int64_t batch_size,
    c10::complex<double>* scratchpad,
    int scratchpadsize) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::getrf_batch,
      queue,
      m,
      n,
      reinterpret_cast<std::complex<double>*>(a),
      lda,
      stride_a,
      ipiv,
      stride_ipiv,
      batch_size,
      reinterpret_cast<std::complex<double>*>(scratchpad),
      scratchpadsize);
}

template <>
void mkl_getrf<c10::complex<float>>(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    c10::complex<float>* a,
    int64_t lda,
    int64_t stride_a,
    int64_t* ipiv,
    int64_t stride_ipiv,
    int64_t batch_size,
    c10::complex<float>* scratchpad,
    int scratchpadsize) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::getrf_batch,
      queue,
      m,
      n,
      reinterpret_cast<std::complex<float>*>(a),
      lda,
      stride_a,
      ipiv,
      stride_ipiv,
      batch_size,
      reinterpret_cast<std::complex<float>*>(scratchpad),
      scratchpadsize);
}

template <typename scalar_t>
void mkl_getrs(
    sycl::queue& queue,
    oneapi::mkl::transpose trans,
    int64_t n,
    int64_t nrhs,
    scalar_t* a,
    int64_t lda,
    int64_t stride_a,
    int64_t* ipiv,
    int64_t stride_ipiv,
    scalar_t* b,
    int64_t ldb,
    int64_t stride_b,
    int64_t batch_size,
    scalar_t* scratchpad,
    int64_t scratchpad_size) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::getrs_batch,
      queue,
      trans,
      n,
      nrhs,
      a,
      lda,
      stride_a,
      ipiv,
      stride_ipiv,
      b,
      ldb,
      stride_b,
      batch_size,
      scratchpad,
      scratchpad_size);
}

template <>
void mkl_getrs<c10::complex<double>>(
    sycl::queue& queue,
    oneapi::mkl::transpose trans,
    int64_t n,
    int64_t nrhs,
    c10::complex<double>* a,
    int64_t lda,
    int64_t stride_a,
    int64_t* ipiv,
    int64_t stride_ipiv,
    c10::complex<double>* b,
    int64_t ldb,
    int64_t stride_b,
    int64_t batch_size,
    c10::complex<double>* scratchpad,
    int64_t scratchpad_size) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::getrs_batch,
      queue,
      trans,
      n,
      nrhs,
      reinterpret_cast<std::complex<double>*>(a),
      lda,
      stride_a,
      ipiv,
      stride_ipiv,
      reinterpret_cast<std::complex<double>*>(b),
      ldb,
      stride_b,
      batch_size,
      reinterpret_cast<std::complex<double>*>(scratchpad),
      scratchpad_size);
}

template <>
void mkl_getrs<c10::complex<float>>(
    sycl::queue& queue,
    oneapi::mkl::transpose trans,
    int64_t n,
    int64_t nrhs,
    c10::complex<float>* a,
    int64_t lda,
    int64_t stride_a,
    int64_t* ipiv,
    int64_t stride_ipiv,
    c10::complex<float>* b,
    int64_t ldb,
    int64_t stride_b,
    int64_t batch_size,
    c10::complex<float>* scratchpad,
    int64_t scratchpad_size) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::getrs_batch,
      queue,
      trans,
      n,
      nrhs,
      reinterpret_cast<std::complex<float>*>(a),
      lda,
      stride_a,
      ipiv,
      stride_ipiv,
      reinterpret_cast<std::complex<float>*>(b),
      ldb,
      stride_b,
      batch_size,
      reinterpret_cast<std::complex<float>*>(scratchpad),
      scratchpad_size);
}

template <typename scalar_t>
void mkl_getri(
    sycl::queue& queue,
    int64_t n,
    scalar_t* a,
    int64_t lda,
    int64_t stride_a,
    int64_t* ipiv,
    int64_t stride_ipiv,
    scalar_t* ainv,
    int64_t ldainv,
    int64_t stride_ainv,
    int64_t batch_size,
    scalar_t* scratchpad,
    int64_t scratchpad_size) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::getri_batch,
      queue,
      n,
      a,
      lda,
      stride_a,
      ipiv,
      stride_ipiv,
      ainv,
      ldainv,
      stride_ainv,
      batch_size,
      scratchpad,
      scratchpad_size);
}

template <>
void mkl_getri<c10::complex<double>>(
    sycl::queue& queue,
    int64_t n,
    c10::complex<double>* a,
    int64_t lda,
    int64_t stride_a,
    int64_t* ipiv,
    int64_t stride_ipiv,
    c10::complex<double>* ainv,
    int64_t ldainv,
    int64_t stride_ainv,
    int64_t batch_size,
    c10::complex<double>* scratchpad,
    int64_t scratchpad_size) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::getri_batch,
      queue,
      n,
      reinterpret_cast<std::complex<double>*>(a),
      lda,
      stride_a,
      ipiv,
      stride_ipiv,
      reinterpret_cast<std::complex<double>*>(ainv),
      ldainv,
      stride_ainv,
      batch_size,
      reinterpret_cast<std::complex<double>*>(scratchpad),
      scratchpad_size);
}

template <>
void mkl_getri<c10::complex<float>>(
    sycl::queue& queue,
    int64_t n,
    c10::complex<float>* a,
    int64_t lda,
    int64_t stride_a,
    int64_t* ipiv,
    int64_t stride_ipiv,
    c10::complex<float>* ainv,
    int64_t ldainv,
    int64_t stride_ainv,
    int64_t batch_size,
    c10::complex<float>* scratchpad,
    int64_t scratchpad_size) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::getri_batch,
      queue,
      n,
      reinterpret_cast<std::complex<float>*>(a),
      lda,
      stride_a,
      ipiv,
      stride_ipiv,
      reinterpret_cast<std::complex<float>*>(ainv),
      ldainv,
      stride_ainv,
      batch_size,
      reinterpret_cast<std::complex<float>*>(scratchpad),
      scratchpad_size);
}

template <typename scalar_t>
int64_t mkl_geqrf_batch_scratchpad_size(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_tau,
    int64_t batch_size) {
  return oneapi::mkl::lapack::geqrf_batch_scratchpad_size<scalar_t>(
      queue, m, n, lda, stride_a, stride_tau, batch_size);
}

template <>
int64_t mkl_geqrf_batch_scratchpad_size<c10::complex<float>>(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_tau,
    int64_t batch_size) {
  return oneapi::mkl::lapack::geqrf_batch_scratchpad_size<std::complex<float>>(
      queue, m, n, lda, stride_a, stride_tau, batch_size);
}

template <>
int64_t mkl_geqrf_batch_scratchpad_size<c10::complex<double>>(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    int64_t lda,
    int64_t stride_a,
    int64_t stride_tau,
    int64_t batch_size) {
  return oneapi::mkl::lapack::geqrf_batch_scratchpad_size<std::complex<double>>(
      queue, m, n, lda, stride_a, stride_tau, batch_size);
}

template <typename scalar_t>
void mkl_geqrf_batch(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    scalar_t* a,
    int64_t lda,
    int64_t stride_a,
    scalar_t* tau,
    int64_t stride_tau,
    int64_t batch_size,
    scalar_t* scratchpad,
    int64_t scratchpadsize) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::geqrf_batch,
      queue,
      m,
      n,
      a,
      lda,
      stride_a,
      tau,
      stride_tau,
      batch_size,
      (scalar_t*)scratchpad,
      scratchpadsize);
}

template <>
void mkl_geqrf_batch<c10::complex<float>>(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    c10::complex<float>* a,
    int64_t lda,
    int64_t stride_a,
    c10::complex<float>* tau,
    int64_t stride_tau,
    int64_t batch_size,
    c10::complex<float>* scratchpad,
    int64_t scratchpadsize) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::geqrf_batch,
      queue,
      m,
      n,
      reinterpret_cast<std::complex<float>*>(a),
      lda,
      stride_a,
      reinterpret_cast<std::complex<float>*>(tau),
      stride_tau,
      batch_size,
      reinterpret_cast<std::complex<float>*>(scratchpad),
      scratchpadsize);
}

template <>
void mkl_geqrf_batch<c10::complex<double>>(
    sycl::queue& queue,
    int64_t m,
    int64_t n,
    c10::complex<double>* a,
    int64_t lda,
    int64_t stride_a,
    c10::complex<double>* tau,
    int64_t stride_tau,
    int64_t batch_size,
    c10::complex<double>* scratchpad,
    int64_t scratchpadsize) {
  DPCPP_ONEMKL_SUBMIT(
      queue,
      oneapi::mkl::lapack::geqrf_batch,
      queue,
      m,
      n,
      reinterpret_cast<std::complex<double>*>(a),
      lda,
      stride_a,
      reinterpret_cast<std::complex<double>*>(tau),
      stride_tau,
      batch_size,
      reinterpret_cast<std::complex<double>*>(scratchpad),
      scratchpadsize);
}
#endif

#ifdef USE_ONEMKL
void error_handle(
    std::vector<int32_t>& infos,
    oneapi::mkl::lapack::batch_error& be) {
  auto errs = be.exceptions();
  auto ids = be.ids();
  for (auto& i : ids) {
    try {
      std::rethrow_exception(errs[i]);
    } catch (oneapi::mkl::lapack::exception e) {
      std::cout << "Cathed lapack exception:"
                << "\nWhat: " << e.what() << "\nInfo: " << e.info()
                << "\nDetail: " << e.detail() << std::endl;
      infos[i] = e.info();
    } catch (sycl::exception e) {
      std::cout << "Catched SYCL exception:"
                << "\nWhat: " << e.what() << "\nInfo: -1" << std::endl;
      infos[i] = -1;
    }
  }
}
#endif

template <typename scalar_t, typename IndexType, bool upper>
struct ApplyTriuTrilKernelFunctor {
  void operator()(sycl::nd_item<1> item) const {
    for (size_t linearIndex = item.get_global_id(0); linearIndex < (size_t)N;
         linearIndex += item.get_global_range()[0]) {
      IndexType batch_id = linearIndex / (self_size_0 * self_size_1);
      IndexType row = (linearIndex % (self_size_0 * self_size_1)) / self_size_1;
      IndexType col = (linearIndex % (self_size_0 * self_size_1)) % self_size_1;

      IndexType src_index =
          batch_id * self_stride + row * self_stride_0 + col * self_stride_1;
      IndexType tgt_index = batch_id * result_stride + row * result_stride_0 +
          col * result_stride_1;

      bool mask = upper ? (col - row >= k) : (col - row <= k);
      result_ptr[tgt_index] = mask ? self_ptr[src_index] : scalar_t(0);
    }
  }
  ApplyTriuTrilKernelFunctor(
      const int64_t k_,
      int64_t N_,
      IndexType self_size_0_,
      IndexType self_size_1_,
      IndexType self_stride_,
      IndexType self_stride_0_,
      IndexType self_stride_1_,
      IndexType result_stride_,
      IndexType result_stride_0_,
      IndexType result_stride_1_,
      scalar_t* result_ptr_,
      scalar_t* self_ptr_)
      : k(k_),
        N(N_),
        self_size_0(self_size_0_),
        self_size_1(self_size_1_),
        self_stride(self_stride_),
        self_stride_0(self_stride_0_),
        self_stride_1(self_stride_1_),
        result_stride(result_stride_),
        result_stride_0(result_stride_0_),
        result_stride_1(result_stride_1_),
        result_ptr(result_ptr_),
        self_ptr(self_ptr_) {}

 private:
  const int64_t k;
  int64_t N;
  IndexType self_size_0;
  IndexType self_size_1;
  IndexType self_stride;
  IndexType self_stride_0;
  IndexType self_stride_1;
  IndexType result_stride;
  IndexType result_stride_0;
  IndexType result_stride_1;
  scalar_t* result_ptr;
  scalar_t* self_ptr;
};

template <typename scalar_t, typename IndexType, bool upper>
void apply_triu_tril(Tensor& result, const Tensor& self, const int64_t k) {
  auto& queue = dpcppGetCurrentQueue();
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  auto N = self.numel();
  int64_t group_size = dpcppMaxWorkGroupSize(dev_id);
  auto num_groups = CeilDiv(N, group_size);
  auto total_items = num_groups * group_size;
  IndexType self_size_0 = (IndexType)self.size(-2);
  IndexType self_size_1 = (IndexType)self.size(-1);
  IndexType self_stride = (IndexType)(self.dim() > 2 ? self.stride(-3) : 1);
  IndexType self_stride_0 = (IndexType)self.stride(-2);
  IndexType self_stride_1 = (IndexType)self.stride(-1);
  IndexType result_stride =
      (IndexType)(result.dim() > 2 ? result.stride(-3) : 1);
  IndexType result_stride_0 = (IndexType)result.stride(-2);
  IndexType result_stride_1 = (IndexType)result.stride(-1);

  scalar_t* result_ptr = (scalar_t*)(result.data_ptr());
  scalar_t* self_ptr = (scalar_t*)(self.data_ptr());

  auto cgf = DPCPP_Q_CGF(cgh) {
    ApplyTriuTrilKernelFunctor<scalar_t, IndexType, upper> kfn(
        k,
        N,
        self_size_0,
        self_size_1,
        self_stride,
        self_stride_0,
        self_stride_1,
        result_stride,
        result_stride_0,
        result_stride_1,
        result_ptr,
        self_ptr);
    // kick off kernel
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<1>(
            sycl::range<1>(total_items), sycl::range<1>(group_size)),
        kfn);
  };

  DPCPP_Q_SUBMIT(queue, cgf);
}

#define TRIU_TRIL_LAMBDA(upper)                                       \
  [&] {                                                               \
    if (torch_ipex::xpu::dpcpp::detail::canUse32BitIndexMath(self)) { \
      apply_triu_tril<scalar_t, int32_t, upper>(result, self, k);     \
    } else {                                                          \
      apply_triu_tril<scalar_t, int64_t, upper>(result, self, k);     \
    }                                                                 \
  }

Tensor& tril_dpcpp_out(Tensor& result, const Tensor& self, int64_t k) {
  if (result.sizes() != self.sizes()) {
    result.resize_as_(self);
  }
  if (self.numel() == 0) {
    return result;
  }

  IPEX_DISPATCH_ALL_TYPES_AND_COMPLEX_AND3(
      at::ScalarType::Half,
      at::ScalarType::Bool,
      at::ScalarType::BFloat16,
      self.scalar_type(),
      "tril",
      TRIU_TRIL_LAMBDA(false));

  return result;
}

Tensor& tril_dpcpp_(Tensor& self, int64_t k) {
  return tril_dpcpp_out(self, self, k);
}

Tensor& triu_dpcpp_out(Tensor& result, const Tensor& self, int64_t k) {
  if (result.sizes() != self.sizes()) {
    result.resize_as_(self);
  }
  if (self.numel() == 0) {
    return result;
  }
  IPEX_DISPATCH_ALL_TYPES_AND_COMPLEX_AND3(
      at::ScalarType::Half,
      at::ScalarType::Bool,
      at::ScalarType::BFloat16,
      self.scalar_type(),
      "triu",
      TRIU_TRIL_LAMBDA(true));

  return result;
}

Tensor& triu_dpcpp_(Tensor& self, int64_t k) {
  return triu_dpcpp_out(self, self, k);
}

template <typename scalar_t>
static void apply_lu_dpcpp_(
    Tensor& self_,
    Tensor& pivots_,
    std::vector<int32_t>& infos_) {
#ifdef USE_ONEMKL
  // do nothing if empty input.
  if (self_.numel() == 0)
    return;
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  int64_t batch_size = native::batchCount(self_);
  int64_t m = self_.size(-2);
  int64_t n = self_.size(-1);
  int64_t lda = m;
  int64_t stride_a = lda * n;
  int64_t stride_ipiv = (m < n) ? m : n;
  scalar_t* a = (scalar_t*)(self_.data_ptr());
  int64_t* ipiv = (int64_t*)(pivots_.data_ptr());
  int64_t scratchpadsize = mkl_getrf_scratchpad<scalar_t>(
      dpcpp_queue, m, n, lda, stride_a, stride_ipiv, batch_size);
  Tensor scratchpad_at = at::empty({scratchpadsize}, self_.options());
  try {
    mkl_getrf<scalar_t>(
        dpcpp_queue,
        m,
        n,
        a,
        lda,
        stride_a,
        ipiv,
        stride_ipiv,
        batch_size,
        (scalar_t*)(scratchpad_at.data_ptr()),
        scratchpadsize);
  } catch (oneapi::mkl::lapack::batch_error be) {
    error_handle(infos_, be);
  }
#else
  AT_ERROR("lu: oneMKL library not found in compilation");
#endif
}

template <typename scalar_t>
static void apply_lu_solve_dpcpp_(
    const Tensor& b_,
    const Tensor& lu_,
    const Tensor& pivots_,
    std::vector<int32_t>& infos_,
    TransposeType t) {
#ifdef USE_ONEMKL
  // do nothing if empty input
  if (lu_.numel() == 0)
    return;
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  int64_t batch_size = native::batchCount(b_);

  auto trans = to_blas(t);
  int64_t n = lu_.size(-2);
  int64_t nrhs = b_.size(-1);
  int64_t lda = lu_.size(-2);
  int64_t stride_a = native::matrixStride(lu_);
  int64_t stride_ipiv = pivots_.size(-1);
  int64_t ldb = b_.size(-2);
  int64_t stride_b = native::matrixStride(b_);

  scalar_t* a = (scalar_t*)(lu_.data_ptr());
  Tensor pivots = pivots_;
  if (pivots_.scalar_type() == at::ScalarType::Int)
    pivots = pivots_.to(kLong);
  int64_t* ipiv = (int64_t*)(pivots.data_ptr());
  scalar_t* b = (scalar_t*)(b_.data_ptr());

  int64_t scratchpadsize = mkl_getrs_scratchpad<scalar_t>(
      dpcpp_queue,
      trans,
      n,
      nrhs,
      lda,
      stride_a,
      stride_ipiv,
      ldb,
      stride_b,
      batch_size);
  Tensor scratchpad_at = at::empty({scratchpadsize}, b_.options());
  try {
    mkl_getrs<scalar_t>(
        dpcpp_queue,
        trans,
        n,
        nrhs,
        a,
        lda,
        stride_a,
        ipiv,
        stride_ipiv,
        b,
        ldb,
        stride_b,
        batch_size,
        (scalar_t*)(scratchpad_at.data_ptr()),
        scratchpadsize);
  } catch (oneapi::mkl::lapack::batch_error be) {
    error_handle(infos_, be);
  }
#else
  AT_ERROR("lu: oneMKL library not found in compilation");
#endif
}

/*
Note: A workaround to align with MKL API to store infos_lu
and infos_getri in vector. For future efficiency concern,
the MKL API needs to accept tensor data_ptr as input and store
the error infos inplace.
*/
template <typename scalar_t>
static void apply_inverse_dpcpp_(
    Tensor& self_,
    Tensor& self_inv_,
    std::vector<int32_t>& infos_lu,
    std::vector<int32_t>& infos_getri) {
#ifdef USE_ONEMKL
  auto req_size = self_.sizes().vec();
  req_size.pop_back();
  Tensor pivots_ = at::empty(req_size, self_.options().dtype(kLong));
  impl::apply_lu_dpcpp_<scalar_t>(self_, pivots_, infos_lu);

  auto& dpcpp_queue = dpcppGetCurrentQueue();
  int64_t batch_size = native::batchCount(self_);

  int64_t n = self_.size(-2);
  // The lda, stride_a and stride_ipiv are assigned 0 when the shape of self_ is
  // [0, 0]. And this function will fail. Aligning with Pytorch cpu, they are
  // assigned 1 when the shape of self_ is [0, 0].
  int64_t lda = std::max<int64_t>(1, n);
  int64_t stride_a = std::max<int64_t>(1, native::matrixStride(self_));
  int64_t stride_ipiv = std::max<int64_t>(1, pivots_.size(-1));

  scalar_t* a = (scalar_t*)(self_.data_ptr());
  int64_t* ipiv = (int64_t*)(pivots_.data_ptr());
  scalar_t* ainv = (scalar_t*)(self_inv_.data_ptr());

  int64_t scratchpadsize = mkl_getri_scratchpad<scalar_t>(
      dpcpp_queue, n, lda, stride_a, stride_ipiv, lda, stride_a, batch_size);
  Tensor scratchpad_at = at::empty({scratchpadsize}, self_.options());
  try {
    mkl_getri<scalar_t>(
        dpcpp_queue,
        n,
        a,
        lda,
        stride_a,
        ipiv,
        stride_ipiv,
        ainv,
        lda,
        stride_a,
        batch_size,
        (scalar_t*)(scratchpad_at.data_ptr()),
        scratchpadsize);
  } catch (oneapi::mkl::lapack::batch_error be) {
    error_handle(infos_getri, be);
  }
#else
  AT_ERROR("lu: oneMKL library not found in compilation");
#endif
}

template <typename scalar_t>
static void apply_geqrf_dpcpp_(
    Tensor& self_,
    Tensor& tau_,
    int64_t m_,
    int64_t n_,
    std::vector<int32_t>& infos_) {
#ifdef USE_ONEMKL
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  int64_t batch_size = native::batchCount(self_);

  int64_t m = m_;
  int64_t n = n_;
  int64_t lda = self_.size(-2);
  int64_t stride_a = native::matrixStride(self_);
  int64_t stride_tau = tau_.size(-1);

  scalar_t* a = (scalar_t*)(self_.data_ptr());
  scalar_t* tau = (scalar_t*)(tau_.data_ptr());

  int64_t scratchpadsize = mkl_geqrf_batch_scratchpad_size<scalar_t>(
      dpcpp_queue, m, n, lda, stride_a, stride_tau, batch_size);
  Tensor scratchpad_at = at::empty({scratchpadsize}, self_.options());
  try {
    mkl_geqrf_batch<scalar_t>(
        dpcpp_queue,
        m,
        n,
        a,
        lda,
        stride_a,
        tau,
        stride_tau,
        batch_size,
        (scalar_t*)(scratchpad_at.data_ptr()),
        scratchpadsize);
  } catch (oneapi::mkl::lapack::batch_error be) {
    error_handle(infos_, be);
  }
#else
  AT_ERROR("lu: oneMKL library not found in compilation");
#endif
}

template <typename scalar_t>
static void apply_ormqr_dpcpp_(
    const Tensor& a_,
    const Tensor& tau_,
    Tensor& c_,
    const int64_t m_,
    const int64_t n_,
    const int64_t k_,
    const bool left_,
    const bool transpose_,
    int64_t& info_) {
#ifdef USE_ONEMKL
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  auto left_right =
      (left_ ? oneapi::mkl::side::left : oneapi::mkl::side::right);
  auto trans =
      (transpose_ ? oneapi::mkl::transpose::trans
                  : oneapi::mkl::transpose::nontrans);
  int64_t m = m_;
  int64_t n = n_;
  int64_t k = k_;
  int64_t lda = (left_ ? m : n);
  int64_t ldc = m;
  scalar_t* a = (scalar_t*)(a_.data_ptr());
  scalar_t* tau = (scalar_t*)(tau_.data_ptr());
  scalar_t* c = (scalar_t*)(c_.data_ptr());

  int64_t scratchpadsize = oneapi::mkl::lapack::ormqr_scratchpad_size<scalar_t>(
      dpcpp_queue, left_right, trans, m, n, k, lda, ldc);
  Tensor scratchpad_at = at::empty({scratchpadsize}, c_.options());
  try {
    DPCPP_ONEMKL_SUBMIT(
        dpcpp_queue,
        oneapi::mkl::lapack::ormqr,
        dpcpp_queue,
        left_right,
        trans,
        m,
        n,
        k,
        a,
        lda,
        tau,
        c,
        ldc,
        (scalar_t*)(scratchpad_at.data_ptr()),
        scratchpadsize);
  } catch (oneapi::mkl::lapack::exception e) {
    std::cout << "Cathed lapack exception:"
              << "\nWhat: " << e.what() << "\nInfo: " << e.info()
              << "\nDetail: " << e.detail() << std::endl;
    info_ = e.info();
  }
#else
  AT_ERROR("lu: oneMKL library not found in compilation");
#endif
}

// do nothing here, only for template specialization
template <typename scalar_t>
void apply_unmqr_dpcpp_(
    const Tensor& a_,
    const Tensor& tau_,
    Tensor& c_,
    const int64_t m_,
    const int64_t n_,
    const int64_t k_,
    const bool left_,
    const bool transpose_,
    int64_t& info_) {}

// unmqr is the complex support for ormqr
template <>
void apply_unmqr_dpcpp_<c10::complex<float>>(
    const Tensor& a_,
    const Tensor& tau_,
    Tensor& c_,
    const int64_t m_,
    const int64_t n_,
    const int64_t k_,
    const bool left_,
    const bool transpose_,
    int64_t& info_) {
#ifdef USE_ONEMKL
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  auto left_right =
      (left_ ? oneapi::mkl::side::left : oneapi::mkl::side::right);
  auto trans =
      (transpose_ ? oneapi::mkl::transpose::trans
                  : oneapi::mkl::transpose::nontrans);
  int64_t m = m_;
  int64_t n = n_;
  int64_t k = k_;
  int64_t lda = (left_ ? m : n);
  int64_t ldc = m;
  auto a = static_cast<std::complex<float>*>(a_.data_ptr());
  auto tau = static_cast<std::complex<float>*>(tau_.data_ptr());
  auto c = static_cast<std::complex<float>*>(c_.data_ptr());

  int64_t scratchpadsize =
      oneapi::mkl::lapack::unmqr_scratchpad_size<std::complex<float>>(
          dpcpp_queue, left_right, trans, m, n, k, lda, ldc);
  Tensor scratchpad_at = at::empty({scratchpadsize}, c_.options());
  try {
    DPCPP_ONEMKL_SUBMIT(
        dpcpp_queue,
        oneapi::mkl::lapack::unmqr,
        dpcpp_queue,
        left_right,
        trans,
        m,
        n,
        k,
        a,
        lda,
        tau,
        c,
        ldc,
        static_cast<std::complex<float>*>(scratchpad_at.data_ptr()),
        scratchpadsize);
  } catch (oneapi::mkl::lapack::exception e) {
    std::cout << "Cathed lapack exception:"
              << "\nWhat: " << e.what() << "\nInfo: " << e.info()
              << "\nDetail: " << e.detail() << std::endl;
    info_ = e.info();
  }
#else
  AT_ERROR("unmqr: oneMKL library not found in compilation");
#endif
}

// unmqr is the complex support for ormqr
template <>
void apply_unmqr_dpcpp_<c10::complex<double>>(
    const Tensor& a_,
    const Tensor& tau_,
    Tensor& c_,
    const int64_t m_,
    const int64_t n_,
    const int64_t k_,
    const bool left_,
    const bool transpose_,
    int64_t& info_) {
#ifdef USE_ONEMKL
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  auto left_right =
      (left_ ? oneapi::mkl::side::left : oneapi::mkl::side::right);
  auto trans =
      (transpose_ ? oneapi::mkl::transpose::trans
                  : oneapi::mkl::transpose::nontrans);
  int64_t m = m_;
  int64_t n = n_;
  int64_t k = k_;
  int64_t lda = (left_ ? m : n);
  int64_t ldc = m;
  auto a = static_cast<std::complex<double>*>(a_.data_ptr());
  auto tau = static_cast<std::complex<double>*>(tau_.data_ptr());
  auto c = static_cast<std::complex<double>*>(c_.data_ptr());

  int64_t scratchpadsize =
      oneapi::mkl::lapack::unmqr_scratchpad_size<std::complex<double>>(
          dpcpp_queue, left_right, trans, m, n, k, lda, ldc);
  Tensor scratchpad_at = at::empty({scratchpadsize}, c_.options());
  try {
    DPCPP_ONEMKL_SUBMIT(
        dpcpp_queue,
        oneapi::mkl::lapack::unmqr,
        dpcpp_queue,
        left_right,
        trans,
        m,
        n,
        k,
        a,
        lda,
        tau,
        c,
        ldc,
        static_cast<std::complex<double>*>(scratchpad_at.data_ptr()),
        scratchpadsize);
  } catch (oneapi::mkl::lapack::exception e) {
    std::cout << "Cathed lapack exception:"
              << "\nWhat: " << e.what() << "\nInfo: " << e.info()
              << "\nDetail: " << e.detail() << std::endl;
    info_ = e.info();
  }
#else
  AT_ERROR("unmqr: oneMKL library not found in compilation");
#endif
}

// Copy from PyTorch fmk. The utils is not compatible with kXPU backend in 1.10
static inline std::tuple<Tensor, Tensor, Tensor> _create_U_S_VT(
    const Tensor& input,
    bool some,
    bool compute_uv) {
  auto sizes = input.sizes().vec();
  int64_t m = input.size(-2), n = input.size(-1);
  auto k = std::min(m, n);

  sizes[input.dim() - 1] = (compute_uv && some) ? k : m;
  auto U_strides =
      at::native::batched_matrix_contiguous_strides(sizes, /*f-contig*=*/true);
  // U should be a column-major or a batch of column-major matrices
  // ... x m x ucol will have strides: ...., ucol, 1
  // We require: ...., 1, m

  Tensor U_empty;
  U_empty = at::empty_strided(sizes, U_strides, input.options());

  sizes[input.dim() - 2] = some ? k : n;
  sizes[input.dim() - 1] = n;
  auto Vh_strides =
      at::native::batched_matrix_contiguous_strides(sizes, /*f-contig*=*/true);
  Tensor VT_empty;
  VT_empty = at::empty_strided(sizes, Vh_strides, input.options());

  sizes.pop_back();
  sizes[input.dim() - 2] = std::min(m, n);
  Tensor S_empty;
  ScalarType dtype = toRealValueType(typeMetaToScalarType(input.dtype()));
  S_empty = at::empty(sizes, input.options().dtype(dtype));
  return std::tuple<Tensor, Tensor, Tensor>(U_empty, S_empty, VT_empty);
}

template <typename scalar_t>
static void apply_triangular_solve(
    Tensor& A,
    Tensor& B,
    bool left,
    bool upper,
    bool transpose,
    bool unitriangular) {
#ifdef USE_ONEMKL
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  oneapi::mkl::transpose trans =
      transpose ? oneapi::mkl::transpose::T : oneapi::mkl::transpose::N;
  oneapi::mkl::uplo uplo = upper ? oneapi::mkl::uplo::U : oneapi::mkl::uplo::L;
  oneapi::mkl::diag diag =
      unitriangular ? oneapi::mkl::diag::U : oneapi::mkl::diag::N;
  oneapi::mkl::side side =
      left ? oneapi::mkl::side::left : oneapi::mkl::side::right;

  auto A_data = A.data_ptr<scalar_t>();
  auto B_data = B.data_ptr<scalar_t>();
  auto A_mat_stride = native::matrixStride(A);
  auto B_mat_stride = native::matrixStride(B);
  auto batch_size = native::batchCount(A);

  auto m = left ? A.size(-1) : B.size(-2);
  auto n = B.size(-1);
  auto lda = std::max<int64_t>(1, A.size(-2));
  auto ldb = std::max<int64_t>(1, B.size(-2));
  scalar_t alpha = 1.;
  for (const auto i : c10::irange(batch_size)) {
    scalar_t* A_working_ptr = &A_data[i * A_mat_stride];
    scalar_t* B_working_ptr = &B_data[i * B_mat_stride];
    DPCPP_ONEMKL_SUBMIT(
        dpcpp_queue,
        oneapi::mkl::blas::column_major::trsm,
        dpcpp_queue,
        side,
        uplo,
        trans,
        diag,
        m,
        n,
        alpha,
        A_working_ptr,
        lda,
        B_working_ptr,
        ldb);
  }
#else
  AT_ERROR("triangular_solve: oneMKL library not found in compilation");
#endif
}

template <>
void apply_triangular_solve<c10::complex<float>>(
    Tensor& A,
    Tensor& B,
    bool left,
    bool upper,
    bool transpose,
    bool unitriangular) {
#ifdef USE_ONEMKL
  oneapi::mkl::transpose trans =
      transpose ? oneapi::mkl::transpose::T : oneapi::mkl::transpose::N;
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  oneapi::mkl::uplo uplo = upper ? oneapi::mkl::uplo::U : oneapi::mkl::uplo::L;
  oneapi::mkl::diag diag =
      unitriangular ? oneapi::mkl::diag::U : oneapi::mkl::diag::N;
  oneapi::mkl::side side =
      left ? oneapi::mkl::side::left : oneapi::mkl::side::right;

  auto A_data = A.data_ptr<c10::complex<float>>();
  auto B_data = B.data_ptr<c10::complex<float>>();
  auto A_mat_stride = native::matrixStride(A);
  auto B_mat_stride = native::matrixStride(B);
  auto batch_size = native::batchCount(A);

  auto m = left ? A.size(-1) : B.size(-2);
  auto n = B.size(-1);
  auto lda = std::max<int64_t>(1, A.size(-2));
  auto ldb = std::max<int64_t>(1, B.size(-2));
  std::complex<float> alpha = 1.f;
  for (const auto i : c10::irange(batch_size)) {
    c10::complex<float>* A_working_ptr = &A_data[i * A_mat_stride];
    c10::complex<float>* B_working_ptr = &B_data[i * B_mat_stride];
    DPCPP_ONEMKL_SUBMIT(
        dpcpp_queue,
        oneapi::mkl::blas::column_major::trsm,
        dpcpp_queue,
        side,
        uplo,
        trans,
        diag,
        m,
        n,
        alpha,
        reinterpret_cast<std::complex<float>*>(A_working_ptr),
        lda,
        reinterpret_cast<std::complex<float>*>(B_working_ptr),
        ldb);
  }
#else
  AT_ERROR("triangular_solve: oneMKL library not found in compilation");
#endif
}

template <>
void apply_triangular_solve<c10::complex<double>>(
    Tensor& A,
    Tensor& B,
    bool left,
    bool upper,
    bool transpose,
    bool unitriangular) {
#ifdef USE_ONEMKL
  oneapi::mkl::transpose trans =
      transpose ? oneapi::mkl::transpose::T : oneapi::mkl::transpose::N;
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  oneapi::mkl::uplo uplo = upper ? oneapi::mkl::uplo::U : oneapi::mkl::uplo::L;
  oneapi::mkl::diag diag =
      unitriangular ? oneapi::mkl::diag::U : oneapi::mkl::diag::N;
  oneapi::mkl::side side =
      left ? oneapi::mkl::side::left : oneapi::mkl::side::right;

  auto A_data = A.data_ptr<c10::complex<double>>();
  auto B_data = B.data_ptr<c10::complex<double>>();
  auto A_mat_stride = native::matrixStride(A);
  auto B_mat_stride = native::matrixStride(B);
  auto batch_size = native::batchCount(A);

  auto m = left ? A.size(-1) : B.size(-2);
  auto n = B.size(-1);
  auto lda = std::max<int64_t>(1, A.size(-2));
  auto ldb = std::max<int64_t>(1, B.size(-2));
  std::complex<double> alpha = 1.;
  for (const auto i : c10::irange(batch_size)) {
    c10::complex<double>* A_working_ptr = &A_data[i * A_mat_stride];
    c10::complex<double>* B_working_ptr = &B_data[i * B_mat_stride];
    DPCPP_ONEMKL_SUBMIT(
        dpcpp_queue,
        oneapi::mkl::blas::column_major::trsm,
        dpcpp_queue,
        side,
        uplo,
        trans,
        diag,
        m,
        n,
        alpha,
        reinterpret_cast<std::complex<double>*>(A_working_ptr),
        lda,
        reinterpret_cast<std::complex<double>*>(B_working_ptr),
        ldb);
  }
#else
  AT_ERROR("triangular_solve: oneMKL library not found in compilation");
#endif
}

} // namespace impl

Tensor& triu_out(const Tensor& self, int64_t diagonal, Tensor& out) {
  impl::triu_dpcpp_out(out, self, diagonal);
  return out;
}

Tensor& tril_out(const Tensor& self, int64_t diagonal, Tensor& out) {
  impl::tril_dpcpp_out(out, self, diagonal);
  return out;
}

Tensor& tril_(Tensor& self, int64_t diagonal) {
  return at::AtenIpexTypeXPU::tril_out(self, diagonal, self);
}

Tensor& triu_(Tensor& self, int64_t diagonal) {
  return at::AtenIpexTypeXPU::triu_out(self, diagonal, self);
}

// Solves a system of linear equations matmul(input, x) = other in-place
static Tensor& linalg_solve_out_info(
    Tensor& result,
    Tensor& infos,
    const Tensor& input,
    const Tensor& other) {
  at::native::checkSameDevice("linalg_solve", result, input);
  at::native::checkSameDevice("linalg_solve", other, input, "other");
  at::native::checkLinalgCompatibleDtype("linalg_solve", result, input);

  TORCH_CHECK(
      input.scalar_type() == other.scalar_type(),
      "input dtype ",
      input.scalar_type(),
      " does not match other dtype ",
      other.scalar_type());

  TORCH_CHECK(
      input.dim() >= 2,
      "input should have at least 2 dimensions, but has ",
      input.dim(),
      " dimensions instead");
  TORCH_CHECK(
      other.dim() >= 1,
      "other should have at least 1 dimension, but has ",
      other.dim(),
      " dimensions instead");

  // Two types of 'other' tensors are supported:
  // - 1-dimensional (1D) tensor or batch of 1D tensors (vector case)
  // - 2-dimensional (2D) tensor or batch of 2D tensors (matrix case)
  // original torch.solve supported only the matrix case, while NumPy works for
  // both cases for the batched input we need to be able to distinguish them
  bool vector_case = at::native::linalg_solve_is_vector_rhs(input, other);

  bool is_batched_column_major = false;
  if (vector_case) {
    is_batched_column_major = result.is_contiguous();
  } else if (!vector_case && result.dim() >= 2) {
    is_batched_column_major = result.transpose(-2, -1).is_contiguous();
  }

  // if 'other' is a batch of 2D tensors, then 'input' can be non-batched and
  // will be broadcasted
  auto expected_shape =
      IntArrayRef(input.sizes().data(), input.dim() - 1); // input.shape[:-1]
  if (!vector_case && other.dim() > 2) {
    expected_shape = other.sizes();
  }

  bool result_equal_expected_shape = result.sizes().equals(expected_shape);
  bool result_input_same_type = (result.scalar_type() == input.scalar_type());

  // if result is not empty and not in batched column major format
  bool copy_needed = (result.numel() != 0 && !is_batched_column_major);
  copy_needed |= !result_input_same_type; // or result does not have the same
                                          // dtype as input
  copy_needed |=
      (result.numel() != 0 &&
       !result_equal_expected_shape); // or result does not have the expected
                                      // shape
  // we have to allocate a temporary tensor
  if (copy_needed) {
    Tensor result_tmp = at::empty({0}, input.options());
    result_tmp = linalg_solve_out_info(result_tmp, infos, input, other);
    resize_output(result, result_tmp.sizes());
    result.copy_(result_tmp);
    return result;
  }
  // else use result's storage directly

  // we need to unsqueeze 'other' because 2-dimensional tensors are expected in
  // the implementation
  Tensor other_ = vector_case ? other.unsqueeze(-1) : other;

  // _linalg_broadcast_batch_dims also includes linearSolveCheckInputs
  // it checks for squareness of 'input' and 'shape' compatibility of 'other'
  // and 'input'
  Tensor other_broadcasted, input_broadcasted;
  std::tie(other_broadcasted, input_broadcasted) =
      at::native::_linalg_broadcast_batch_dims(other_, input, "linalg_solve");

  auto squeezed_other_broadcasted = at::squeeze(other_broadcasted, -1);
  auto squeezed_result_shape = squeezed_other_broadcasted.sizes();

  // if result has no elements we can modify it
  if (result.numel() == 0) {
    if (vector_case) {
      result.resize_(squeezed_result_shape);
    } else {
      at::native::resize_as_(
          result,
          other_broadcasted.transpose(-2, -1),
          MemoryFormat::Contiguous);
      result.transpose_(-2, -1);
    }
  }

  auto expected_result_shape =
      vector_case ? squeezed_result_shape : other_broadcasted.sizes();
  TORCH_INTERNAL_ASSERT(result.sizes().equals(expected_result_shape));
  TORCH_INTERNAL_ASSERT(result.scalar_type() == input.scalar_type());
  TORCH_INTERNAL_ASSERT(result.device() == input.device());

  // result tensor must be in batched column major order (Fortran contiguous)
  // for 2D inputs or C contiguous for 1D input
  if (vector_case) {
    TORCH_INTERNAL_ASSERT(result.is_contiguous());
  } else {
    TORCH_INTERNAL_ASSERT(result.transpose(-2, -1).is_contiguous());
  }

  // for 1-dimensional 'other', we need to unsqueeze the result before passing
  // to "apply_solve"
  if (vector_case) {
    result = result.unsqueeze_(-1);
  }

  // lu_stub+lu_solve_stub perform calculations in-place and 'result' must be a
  // copy of 'other_broadcasted'
  result.copy_(other_broadcasted);

  auto input_working_copy =
      at::native::cloneBatchedColumnMajor(input_broadcasted);

  infos.resize_({std::max<int64_t>(1, native::batchCount(input_broadcasted))})
      .zero_();
  std::vector<int32_t> infos_vec_1(native::batchCount(input_broadcasted), 0);
  std::vector<int32_t> infos_vec_2(native::batchCount(input_broadcasted), 0);
  // compute the LU factorization of 'input_working_copy'
  auto pivots_shape =
      IntArrayRef(input_broadcasted.sizes().data(), input_broadcasted.dim() - 2)
          .vec(); // input_broadcasted.shape[:-2]
  pivots_shape.push_back(std::min(input.size(-2), input.size(-1)));
  Tensor pivots = at::empty(pivots_shape, input.options().dtype(kLong));
  IPEX_DISPATCH_FLOATING_AND_COMPLEX_TYPES(
      input_working_copy.scalar_type(), "linalg_solve_dpcpp", [&] {
        impl::apply_lu_dpcpp_<scalar_t>(
            input_working_copy, pivots, infos_vec_1);
        // solve the linear system using the LU factorization
        impl::apply_lu_solve_dpcpp_<scalar_t>(
            result,
            input_working_copy,
            pivots,
            infos_vec_2,
            TransposeType::NoTranspose);
      });

  std::copy(
      infos_vec_1.begin(),
      infos_vec_1.end(),
      infos.template data_ptr<int32_t>());

  at::_linalg_check_errors(
      infos, "lu_solve_dpcpp", input_working_copy.dim() == 2);

  // for 1-dimensional 'other', we need to squeeze the result after
  // "apply_solve"
  if (vector_case) {
    result = result.squeeze_(-1);
  }

  return result;
}

Tensor _lu_solve_helper(
    const Tensor& self,
    const Tensor& LU_data,
    const Tensor& LU_pivots) {
  auto self_working_copy = native::cloneBatchedColumnMajor(self);
  auto LU_data_working_copy = native::cloneBatchedColumnMajor(LU_data);
  auto LU_pivots_working_copy =
      LU_pivots.is_contiguous() ? LU_pivots : LU_pivots.contiguous();
  // FIXME: oneMKL only support int64_t datatype of pivots
  LU_pivots_working_copy = LU_pivots.to(kLong);
  auto infos_tensor = at::zeros(
      native::batchCount(self),
      self.options().dtype(kInt).device(DeviceType::CPU));
  std::vector<int32_t> infos(native::batchCount(self), 0);

  if (self.numel() == 0 || LU_data.numel() == 0) {
    return at::zeros_like(self, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
  }
  IPEX_DISPATCH_FLOATING_AND_COMPLEX_TYPES(
      self.scalar_type(), "lu_solve_dpcpp", [&] {
        impl::apply_lu_solve_dpcpp_<scalar_t>(
            self_working_copy,
            LU_data_working_copy,
            LU_pivots_working_copy,
            infos,
            TransposeType::NoTranspose);
      });

  std::copy(
      infos.begin(), infos.end(), infos_tensor.template data_ptr<int32_t>());
  at::_linalg_check_errors(infos_tensor, "lu_solve_dpcpp", self.dim() == 2);

  return self_working_copy;
}

Tensor lu_solve(
    const Tensor& self,
    const Tensor& LU_data,
    const Tensor& LU_pivots) {
  TORCH_CHECK(
      self.dim() >= 2,
      "b should have at least 2 dimensions, but has ",
      self.dim(),
      " dimensions instead");
  TORCH_CHECK(
      LU_data.dim() >= 2,
      "LU_data should have at least 2 dimensions, but has ",
      LU_data.dim(),
      " dimensions instead");
  TORCH_CHECK(
      LU_pivots.size(-1) == LU_data.size(-1),
      "Number of pivots per batch should be same as the dimension of the matrix");
  TORCH_CHECK(
      LU_pivots.device() == LU_data.device(),
      "Expected LU_pivots and LU_data to be on the same device, "
      "but found LU_pivots on ",
      LU_pivots.device(),
      " and LU_data on ",
      LU_data.device(),
      " instead");

  IntArrayRef pivots_sizes(LU_pivots.sizes().data(), LU_pivots.dim() - 1);
  IntArrayRef lu_sizes(LU_data.sizes().data(), LU_data.dim() - 2);
  TORCH_CHECK(
      pivots_sizes == lu_sizes,
      "batch dimensions of LU_pivots doesn't match batch dimensions of LU_data");

  Tensor self_broadcasted, LU_data_broadcasted;
  std::tie(self_broadcasted, LU_data_broadcasted) =
      native::_linalg_broadcast_batch_dims(self, LU_data, "lu_solve_dpcpp");

  IntArrayRef new_pivots_sizes(
      LU_data_broadcasted.sizes().data(), LU_data_broadcasted.dim() - 1);
  Tensor LU_pivots_broadcasted = LU_pivots.expand(new_pivots_sizes);
  return at::AtenIpexTypeXPU::_lu_solve_helper(
      self_broadcasted, LU_data_broadcasted, LU_pivots_broadcasted);
}

Tensor& lu_solve_out(
    const Tensor& self,
    const Tensor& LU_data,
    const Tensor& LU_pivots,
    Tensor& out) {
  Tensor out_tmp = at::AtenIpexTypeXPU::lu_solve(self, LU_data, LU_pivots);
  out.resize_as_(out_tmp).copy_(out_tmp);
  return out;
}

Tensor _inverse_helper(const Tensor& self) {
  auto infos_tensor = at::zeros(
      native::batchCount(self),
      self.options().dtype(kInt).device(DeviceType::CPU));
  std::vector<int32_t> infos_lu_vec(native::batchCount(self), 0);
  std::vector<int32_t> infos_getri_vec(native::batchCount(self), 0);

  auto self_working_copy = native::cloneBatchedColumnMajor(self);
  auto self_inv_working_copy = native::cloneBatchedColumnMajor(self);
  IPEX_DISPATCH_FLOATING_AND_COMPLEX_TYPES(
      self.scalar_type(), "inverse_dpcpp", [&] {
        impl::apply_inverse_dpcpp_<scalar_t>(
            self_working_copy,
            self_inv_working_copy,
            infos_lu_vec,
            infos_getri_vec);
      });

  std::copy(
      infos_lu_vec.begin(),
      infos_lu_vec.end(),
      infos_tensor.template data_ptr<int32_t>());
  at::_linalg_check_errors(infos_tensor, "infos_lu_vec", self.dim() == 2);
  std::copy(
      infos_getri_vec.begin(),
      infos_getri_vec.end(),
      infos_tensor.template data_ptr<int32_t>());
  at::_linalg_check_errors(infos_tensor, "infos_getri_vec", self.dim() == 2);

  return self_inv_working_copy;
}

Tensor inverse(const Tensor& self) {
  if (self.numel() == 0) {
    return at::empty_like(self, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
  }
  native::squareCheckInputs(self, "inverse");
  return at::AtenIpexTypeXPU::_inverse_helper(self);
}

Tensor& inverse_out(const Tensor& self, Tensor& out) {
  if (self.size(-1) == 0) {
    out.resize_as_(self);
    return out;
  }
  out.copy_(at::AtenIpexTypeXPU::inverse(self));
  return out;
}

// A type dispatching helper function for 'apply_inverse_dpcpp_'.
Tensor& _linalg_inv_out_helper_(
    Tensor& result,
    Tensor& infos_lu,
    Tensor& infos_getri) {
  /*
  [Note:] Current mkl API `getrf_batch` and `getri_batch` does not accept
  info_arrays as input to store the error infos, instead, the errors are throwed
  out as exceptions. As a workaround, we store the errors in `vector<int64_t>
  infos`, and convert the vectors to tensors.

  'infos_lu' is for holding LU errors, and 'infos_getri' is for holding getri
  errors

  The `infos_lu` and `infos_getri` are following:

  = 0: successful exit
  < 0: if INFO = -i, the i-th argument had an illegal value or another error
  occured, such as memory allocation failed.
  > 0: if INFO = i, U(i,i) is exactly zero.
      The factorization has been completed, but the factor U is exactly
      singular, and division by zero will occur if it is used to solve a
      system of equation.
  */

  std::vector<int32_t> infos_lu_vec(native::batchCount(result), 0);
  std::vector<int32_t> infos_getri_vec(native::batchCount(result), 0);
  auto self_inv_working_copy = native::cloneBatchedColumnMajor(result);
  IPEX_DISPATCH_FLOATING_AND_COMPLEX_TYPES(
      result.scalar_type(), "linalg_inv_out_dpcpp", [&] {
        impl::apply_inverse_dpcpp_<scalar_t>(
            self_inv_working_copy, result, infos_lu_vec, infos_getri_vec);
      });
  // Needs to handle the copy for scalar tensor separately.
  // Because the copy from 1D tensor to 0D scalar mismatch.
  auto expected_info_shape =
      IntArrayRef(result.sizes().cbegin(), result.sizes().cend() - 2);

  infos_lu.copy_(at::from_blob(
      (int32_t*)(infos_lu_vec.data()),
      expected_info_shape,
      c10::toRealValueType(infos_lu.scalar_type())));
  infos_getri.copy_(at::from_blob(
      (int32_t*)(infos_getri_vec.data()),
      expected_info_shape,
      c10::toRealValueType(infos_getri.scalar_type())));

  return result;
}

std::tuple<Tensor, Tensor> geqrf(const Tensor& self) {
  TORCH_CHECK(
      self.dim() >= 2,
      "input should have at least 2 dimensions. but has ",
      self.dim(),
      " dimensions instead");
  int64_t m = self.size(-2), n = self.size(-1);
  auto req_size = self.sizes().vec();
  req_size.pop_back();
  req_size[self.dim() - 2] = std::min(m, n);
  if (self.numel() == 0) {
    return std::tuple<Tensor, Tensor>(
        at::empty(self.sizes().vec(), self.options()),
        at::empty(req_size, self.options()));
  }

  std::vector<int32_t> infos(native::batchCount(self), 0);
  Tensor self_working_copy = native::cloneBatchedColumnMajor(self);
  Tensor tau_working_copy = at::empty(req_size, self.options());

  IPEX_DISPATCH_FLOATING_AND_COMPLEX_TYPES(
      self.scalar_type(), "geqrf_dpcpp", [&] {
        impl::apply_geqrf_dpcpp_<scalar_t>(
            self_working_copy, tau_working_copy, m, n, infos);
      });
  return std::tuple<Tensor, Tensor>(self_working_copy, tau_working_copy);
}

std::tuple<Tensor&, Tensor&> geqrf_out(
    const Tensor& self,
    Tensor& a,
    Tensor& tau) {
  TORCH_CHECK(
      self.dim() >= 2,
      "input should have at least 2 dimensions. but has ",
      self.dim(),
      " dimensions instead");
  TORCH_CHECK(self.numel() != 0, "input must not be empty");

  Tensor a_tmp, tau_tmp;
  std::cout << "inside geqrf\n";
  std::tie(a_tmp, tau_tmp) = at::AtenIpexTypeXPU::geqrf(self);
  a.resize_as_(a_tmp).copy_(a_tmp);
  tau.resize_as_(tau_tmp).copy_(tau_tmp);
  return std::tuple<Tensor&, Tensor&>(a, tau);
}

Tensor ormqr(
    const Tensor& self,
    const Tensor& input2,
    const Tensor& input3,
    bool left,
    bool transpose) {
  std::cout << "inside ormqr\n";
  TORCH_CHECK(
      self.dim() >= 2, "torch.ormqr: input must have at least 2 dimensions.");
  TORCH_CHECK(
      input3.dim() >= 2, "torch.ormqr: other must have at least 2 dimensions.");

  int64_t left_size_condition = left ? -2 : -1;
  TORCH_CHECK(
      input3.size(left_size_condition) >= input2.size(-1),
      "torch.ormqr: other.shape[",
      left_size_condition,
      "] must be greater than or equal to tau.shape[-1]");
  TORCH_CHECK(
      input3.size(left_size_condition) == self.size(-2),
      "torch.ormqr: other.shape[",
      left_size_condition,
      "] must be equal to input.shape[-2]");
  TORCH_CHECK(
      self.dim() - input2.dim() == 1,
      "torch.ormqr: ",
      "Expected tau to have one dimension less than input, but got tau.ndim equal to ",
      input2.dim(),
      " and input.ndim is equal to ",
      self.dim());
  TORCH_CHECK(
      self.dim() == input3.dim(),
      "torch.ormqr: ",
      "Expected other to have the same number of dimensions as input, but got other.ndim equal to ",
      input3.dim(),
      " and input.ndim is equal to ",
      self.dim());

  if (self.dim() > 2) {
    auto expected_batch_shape =
        IntArrayRef(self.sizes().data(), self.dim() - 2); // self.shape[:-2]
    auto actual_batch_tau_shape = IntArrayRef(
        input2.sizes().data(), input2.dim() - 1); // input2.shape[:-1]
    TORCH_CHECK(
        actual_batch_tau_shape.equals(expected_batch_shape),
        "torch.ormqr: Expected batch dimensions of tau to be equal to input.shape[:-2], but got ",
        actual_batch_tau_shape);

    auto actual_batch_other_shape = IntArrayRef(
        input3.sizes().data(), input3.dim() - 2); // input3.shape[:-2]
    TORCH_CHECK(
        actual_batch_other_shape.equals(expected_batch_shape),
        "torch.ormqr: Expected batch dimensions of other to be equal to input.shape[:-2], but got ",
        actual_batch_other_shape);
  }

  TORCH_CHECK(
      input2.scalar_type() == self.scalar_type(),
      "torch.ormqr: Expected input and tau to have the same dtype, but input has dtype",
      self.scalar_type(),
      " and tau has dtype ",
      input2.scalar_type());
  TORCH_CHECK(
      input3.scalar_type() == self.scalar_type(),
      "torch.ormqr: Expected input and other to have the same dtype, but input has dtype",
      self.scalar_type(),
      " and other has dtype ",
      input3.scalar_type());

  native::checkSameDevice("torch.ormqr", input2, self, "tau");
  native::checkSameDevice("torch.ormqr", input3, self, "other");

  int64_t infos = 0;
  int64_t m = input3.size(0), n = input3.size(1), k = input2.size(-1);
  auto c_working_copy = native::cloneBatchedColumnMajor(input3);

  native::checkSameDevice("torch.ormqr", c_working_copy, self);

  TORCH_CHECK(
      c_working_copy.scalar_type() == self.scalar_type(),
      "torch.ormqr: Expected input and result to have the same dtype, but input has dtype",
      self.scalar_type(),
      " and result has dtype ",
      c_working_copy.scalar_type());
  if (self.is_complex()) {
    IPEX_DISPATCH_COMPLEX_TYPES(self.scalar_type(), "ormqr_dpcpp", [&] {
      impl::apply_unmqr_dpcpp_<scalar_t>(
          self,
          input2,
          c_working_copy,
          m,
          n,
          std::min(m, k),
          left,
          transpose,
          infos);
    });
  } else {
    IPEX_DISPATCH_FLOATING_TYPES(self.scalar_type(), "ormqr_dpcpp", [&] {
      impl::apply_ormqr_dpcpp_<scalar_t>(
          self,
          input2,
          c_working_copy,
          m,
          n,
          std::min(m, k),
          left,
          transpose,
          infos);
    });
  }

  return c_working_copy;
}

Tensor& ormqr_out(
    const Tensor& self,
    const Tensor& input2,
    const Tensor& input3,
    bool left,
    bool transpose,
    Tensor& out) {
  TORCH_CHECK(
      self.dim() >= 2, "torch.ormqr: input must have at least 2 dimensions.");
  TORCH_CHECK(
      input3.dim() >= 2, "torch.ormqr: other must have at least 2 dimensions.");

  int64_t left_size_condition = left ? -2 : -1;
  TORCH_CHECK(
      input3.size(left_size_condition) >= input2.size(-1),
      "torch.ormqr: other.shape[",
      left_size_condition,
      "] must be greater than or equal to tau.shape[-1]");

  TORCH_CHECK(
      input3.size(left_size_condition) == self.size(-2),
      "torch.ormqr: other.shape[",
      left_size_condition,
      "] must be equal to input.shape[-2]");

  TORCH_CHECK(
      self.dim() - input2.dim() == 1,
      "torch.ormqr: ",
      "Expected tau to have one dimension less than input, but got tau.ndim equal to ",
      input2.dim(),
      " and input.ndim is equal to ",
      self.dim());
  TORCH_CHECK(
      self.dim() == input3.dim(),
      "torch.ormqr: ",
      "Expected other to have the same number of dimensions as input, but got other.ndim equal to ",
      input3.dim(),
      " and input.ndim is equal to ",
      self.dim());

  if (self.dim() > 2) {
    auto expected_batch_shape =
        IntArrayRef(self.sizes().data(), self.dim() - 2); // self.shape[:-2]
    auto actual_batch_tau_shape = IntArrayRef(
        input2.sizes().data(), input2.dim() - 1); // input2.shape[:-1]
    TORCH_CHECK(
        actual_batch_tau_shape.equals(expected_batch_shape),
        "torch.ormqr: Expected batch dimensions of tau to be equal to input.shape[:-2], but got ",
        actual_batch_tau_shape);

    auto actual_batch_other_shape = IntArrayRef(
        input3.sizes().data(), input3.dim() - 2); // input3.shape[:-2]
    TORCH_CHECK(
        actual_batch_other_shape.equals(expected_batch_shape),
        "torch.ormqr: Expected batch dimensions of other to be equal to input.shape[:-2], but got ",
        actual_batch_other_shape);
  }

  TORCH_CHECK(
      input2.scalar_type() == self.scalar_type(),
      "torch.ormqr: Expected input and tau to have the same dtype, but input has dtype",
      self.scalar_type(),
      " and tau has dtype ",
      input2.scalar_type());
  TORCH_CHECK(
      input3.scalar_type() == self.scalar_type(),
      "torch.ormqr: Expected input and other to have the same dtype, but input has dtype",
      self.scalar_type(),
      " and other has dtype ",
      input3.scalar_type());
  TORCH_CHECK(
      out.scalar_type() == self.scalar_type(),
      "torch.ormqr: Expected input and result to have the same dtype, but input has dtype",
      self.scalar_type(),
      " and result has dtype ",
      out.scalar_type());

  native::checkSameDevice("torch.ormqr", input2, self, "tau");
  native::checkSameDevice("torch.ormqr", input3, self, "other");
  native::checkSameDevice("torch.ormqr", out, self);

  if (self.size(-1) == 0) {
    out.resize_as_(input3);
    return out;
  }
  out.resize_as_(input3).copy_(
      at::AtenIpexTypeXPU::ormqr(self, input2, input3, left, transpose));
  return out;
}

std::tuple<Tensor, Tensor> _triangular_solve_helper(
    Tensor& result,
    Tensor& clone_input,
    Tensor& infos,
    const Tensor& input,
    const Tensor& other,
    bool upper,
    bool transpose,
    bool unitriangular) {
  // These internal asserts make explicit the assumptions in the implementation
  // Error check with the actual error messages are done on the higher level of
  // the hierarchy of calls
  TORCH_INTERNAL_ASSERT(input.dim() >= 2);
  TORCH_INTERNAL_ASSERT(input.size(-2) == input.size(-1));

  TORCH_INTERNAL_ASSERT(input.device() == other.device());
  TORCH_INTERNAL_ASSERT(input.device() == result.device());
  TORCH_INTERNAL_ASSERT(input.device() == clone_input.device());
  TORCH_INTERNAL_ASSERT(input.device() == infos.device());

  TORCH_INTERNAL_ASSERT(input.scalar_type() == other.scalar_type());
  TORCH_INTERNAL_ASSERT(input.scalar_type() == result.scalar_type());
  TORCH_INTERNAL_ASSERT(input.scalar_type() == clone_input.scalar_type());

  TORCH_INTERNAL_ASSERT(infos.scalar_type() == at::kInt);
  TORCH_INTERNAL_ASSERT(
      infos.numel() == std::max<int64_t>(1, native::batchCount(input)));
  TORCH_INTERNAL_ASSERT(infos.is_contiguous());

  // if 'result' has no elements we can modify it
  if (result.numel() == 0) {
    result.resize_(other.transpose(-2, -1).sizes(), MemoryFormat::Contiguous);
    result.transpose_(
        -2, -1); // make 'result' to have Fortran contiguous memory layout
  }

  // if 'clone_input' has no elements we can modify it
  if (clone_input.numel() == 0) {
    clone_input.resize_(
        input.transpose(-2, -1).sizes(), MemoryFormat::Contiguous);
    clone_input.transpose_(-2, -1);
  }

  // 'result' and 'clone_input' must be in batched column major order
  TORCH_INTERNAL_ASSERT(result.transpose(-2, -1).is_contiguous());
  TORCH_INTERNAL_ASSERT(clone_input.transpose(-2, -1).is_contiguous());

  TORCH_INTERNAL_ASSERT(result.sizes().equals(other.sizes()));
  TORCH_INTERNAL_ASSERT(clone_input.sizes().equals(input.sizes()));
  result.copy_(other);
  clone_input.copy_(input);

  IPEX_DISPATCH_FLOATING_AND_COMPLEX_TYPES(
      clone_input.scalar_type(), "triangular_solve_xpu", [&] {
        impl::apply_triangular_solve<scalar_t>(
            clone_input,
            result,
            /*left=*/true,
            upper,
            transpose,
            unitriangular);
      });

  return std::tuple<Tensor, Tensor>(result, clone_input);
}

// Supports arbitrary batch dimensions for self and A
std::tuple<Tensor, Tensor> triangular_solve(
    const Tensor& self,
    const Tensor& A,
    bool upper,
    bool transpose,
    bool unitriangular) {
  TORCH_CHECK(
      self.dim() >= 2,
      "torch.triangular_solve: Expected b to have at least 2 dimensions, but it has ",
      self.dim(),
      " dimensions instead");
  TORCH_CHECK(
      A.dim() >= 2,
      "torch.triangular_solve: Expected A to have at least 2 dimensions, but it has ",
      A.dim(),
      " dimensions instead");
  Tensor self_broadcasted, A_broadcasted;
  std::tie(self_broadcasted, A_broadcasted) =
      native::_linalg_broadcast_batch_dims(self, A, "triangular_solve");

  Tensor result = at::empty({0}, self.options());
  Tensor clone_A = at::empty({0}, self.options());
  Tensor infos = at::zeros(
      {std::max<int64_t>(1, native::batchCount(self_broadcasted))},
      self.options().dtype(kInt));

  at::AtenIpexTypeXPU::_triangular_solve_helper(
      result,
      clone_A,
      infos,
      A_broadcasted,
      self_broadcasted,
      upper,
      transpose,
      unitriangular);

  // TODO: need to rebase 1.13
  // if (self_broadcasted.dim() > 2) {
  //   native::batchCheckErrors(infos, "triangular_solve");
  // } else {
  //   native::singleCheckErrors(infos.item().toInt(), "triangular_solve");
  // }

  return std::tuple<Tensor, Tensor>(result, clone_A);
}

std::tuple<Tensor&, Tensor&> triangular_solve_out(
    const Tensor& self,
    const Tensor& A,
    bool upper,
    bool transpose,
    bool unitriangular,
    Tensor& result,
    Tensor& clone_A) {
  Tensor result_tmp, clone_A_tmp;
  std::tie(result_tmp, clone_A_tmp) = at::AtenIpexTypeXPU::triangular_solve(
      self, A, upper, transpose, unitriangular);
  result.resize_as_(result_tmp).copy_(result_tmp);
  clone_A.resize_as_(clone_A_tmp).copy_(clone_A_tmp);
  return std::tuple<Tensor&, Tensor&>(result, clone_A);
}

std::tuple<Tensor, Tensor> linalg_eig(const Tensor& input) {
  return at::native::linalg_eig(input);
}

std::tuple<Tensor&, Tensor&> linalg_eig_out(
    const Tensor& input,
    Tensor& values,
    Tensor& vectors) {
  auto input_tmp = input.cpu();
  // fall back to CPU
  // 1, mkl doesn't have GPU interface for GEEV routine. and Due to this lack of
  // uniqueness, different hardware and software may compute different
  // eigenvectors.
  // 2, we will try to dep on IPEX oneMKL package as long as if it supports CPU
  // device
  // 3, magma CPU is potential path, as well

  auto options = input.options().device(at::kCPU);
  ScalarType values_type = input.scalar_type();
  ScalarType vectors_type = input.scalar_type();
  if (!input.is_complex()) {
    // for real-valued input we can have either real- or complex-valued output
    ScalarType input_complex_dtype = toComplexType(input.scalar_type());
    values_type = values.is_complex() ? input_complex_dtype : values_type;
    vectors_type = vectors.is_complex() ? input_complex_dtype : vectors_type;
  }
  Tensor values_tmp = at::empty({0}, options.dtype(values_type));
  Tensor vectors_tmp = at::empty({0}, options.dtype(vectors_type));
  std::tie(values_tmp, vectors_tmp) =
      at::native::linalg_eig_out(input_tmp, values_tmp, vectors_tmp);
  resize_output(values, values_tmp.sizes());
  resize_output(vectors, vectors_tmp.sizes());
  values.copy_(values_tmp);
  vectors.copy_(vectors_tmp);
  return std::tuple<Tensor&, Tensor&>(values, vectors);
}

Tensor _linalg_eigvals(const Tensor& input) {
  ScalarType complex_dtype = toComplexType(input.scalar_type());
  Tensor values = at::empty({0}, input.options().dtype(complex_dtype));
  linalg_eigvals_out(input, values);
  return values;
}

Tensor& linalg_eigvals_out(const Tensor& input, Tensor& values) {
  native::squareCheckInputs(input, "linalg.eigvals");

  // unlike NumPy for real-valued inputs the output is always complex-valued
  native::checkLinalgCompatibleDtype(
      "torch.linalg.eigvals",
      values.scalar_type(),
      toComplexType(input.scalar_type()),
      "eigenvalues");
  native::checkSameDevice("torch.linalg.eigvals", values, input, "eigenvalues");
  // MAGMA doesn't have GPU interface for GEEV routine, it requires inputs to be
  // on CPU
  auto options = input.options().device(at::kCPU);
  auto infos = at::zeros(
      {std::max<int64_t>(1, native::batchCount(input))}, options.dtype(kInt));

  bool values_expected_type =
      (values.scalar_type() == toComplexType(input.scalar_type()));

  auto expected_values_shape =
      IntArrayRef(input.sizes().data(), input.dim() - 1); // input.shape[:-1]
  bool values_equal_expected_shape =
      values.sizes().equals(expected_values_shape);

  // if result is not empty and not in batched column major format
  bool values_tmp_needed = (values.numel() != 0 && !values.is_contiguous());
  // or result does not have the expected shape
  values_tmp_needed |= (values.numel() != 0 && !values_equal_expected_shape);
  // or result does not have the expected dtype
  values_tmp_needed |= !values_expected_type;
  // we will allocate a temporary tensor and do the copy

  // because MAGMA's GEEV takes CPU inputs and returns CPU outputs
  // 'values' tensor that is on GPU device can't be used directly
  values_tmp_needed |= (!values.is_cpu());

  // determine the appropriate scalar_type for the temporary tensors
  ScalarType values_type = input.scalar_type();
  if (!input.is_complex()) {
    // for real-valued input we can have either real- or complex-valued output
    ScalarType input_complex_dtype = toComplexType(input.scalar_type());
    values_type = values.is_complex() ? input_complex_dtype : values_type;
  }

  Tensor vectors =
      at::empty({0}, options.dtype(toComplexType(input.scalar_type())));
  if (values_tmp_needed) {
    Tensor values_tmp = at::empty({0}, options.dtype(values_type));
    std::tie(values_tmp, std::ignore) =
        linalg_eig_out(input, values_tmp, vectors);
    at::native::resize_output(values, values_tmp.sizes());
    values.copy_(values_tmp);
  } else { // use 'values' storage directly
    std::tie(values, std::ignore) = linalg_eig_out(input, values, vectors);
  }

  // Now check LAPACK/MAGMA error codes
  at::_linalg_check_errors(infos, "torch.linalg.eigvals", input.dim() == 2);
  return values;
}

Tensor _det_lu_based_helper_backward_helper(
    const Tensor& det_grad,
    const Tensor& det,
    const Tensor& self,
    const Tensor& lu,
    const Tensor& pivs) {
  auto eps = at::native::_get_epsilon(c10::toRealValueType(self.scalar_type()));
  auto n = self.size(-1);
  auto eps_tensor = at::tensor(eps, self.options());
  auto condition_diagonal = [&](const Tensor& x) {
    auto x_diag = x.diagonal(0, -2, -1);
    auto x_diag_conditioned = at::where(x_diag == 0.0, eps_tensor, x_diag);
    x_diag.copy_(x_diag_conditioned);
  };

  // create a matrix d := (det_grad * det.conj()) I
  // NOTE: we do not use the shorter version
  // auto d = at::zeros_like(self);
  // d.diagonal(0, -2, -1).copy_((det_grad * det.conj()).unsqueeze(-1));
  // to avoid in-place operations to eliminate potential issues with Vmap
  auto det_expanded_sizes = det.sizes().vec();
  det_expanded_sizes.push_back(n);
  auto d_diag = det_grad * det.conj();
  auto d = at::diag_embed(d_diag.unsqueeze(-1).expand(det_expanded_sizes));
  // make sure that d is Fortran-contiguous. The transposition is sufficient as
  // d is a diagonal square matrix
  d = d.transpose(-2, -1);

  // we want to condition the diagonal of the lu Tensor, but it is not allowed
  // to modify arguments of backward functions in-place, hence the cloning.
  auto lu_clone = lu.clone();
  condition_diagonal(lu_clone);

  auto trans = self.is_complex() ? TransposeType::ConjTranspose
                                 : TransposeType::Transpose;
  auto infos_tensor = at::zeros(
      native::batchCount(d),
      self.options().dtype(kInt).device(DeviceType::CPU));
  std::vector<int32_t> infos(native::batchCount(d), 0);

  // d is modified in-place and will contain the result
  IPEX_DISPATCH_FLOATING_AND_COMPLEX_TYPES(
      d.scalar_type(), "_det_lu_based_helper_backward_helper", [&] {
        impl::apply_lu_solve_dpcpp_<scalar_t>(d, lu_clone, pivs, infos, trans);
      });

  std::copy(
      infos.begin(), infos.end(), infos_tensor.template data_ptr<int32_t>());
  at::_linalg_check_errors(
      infos_tensor, "_det_lu_based_helper_backward_helper", self.dim() == 2);

  return d;
}

// As P is a permutation matrix
// det(P) = 1 if it's an even permutation and det(P) = -1 if it's an odd
// permutation
Tensor lu_det_P(const Tensor& pivots) {
  return (at::arange(1, pivots.size(-1) + 1, pivots.options()) != pivots)
      .sum(-1, /*keepdim=*/false, /*dtype=*/at::kLong)
      .fmod_(2)
      // take 0 to 1 and 1 to -1
      .mul_(-2)
      .add_(1);
}

std::tuple<Tensor&, Tensor&, Tensor&> _linalg_det_out(
    const Tensor& A,
    Tensor& det,
    Tensor& LU,
    Tensor& pivots) {
  auto shape = A.sizes();
  auto ndim = shape.size();

  // det
  auto det_new = set_contiguous(det, shape.slice(0, ndim - 2), A.options());
  Tensor det_use = C10_UNLIKELY(det_new.has_value()) ? det_new.value() : det;

  // LU
  auto LU_strides =
      at::native::batched_matrix_contiguous_strides(shape, /*f-contig*=*/true);
  auto LU_new = set_strided(LU, shape, LU_strides, A.options());
  Tensor LU_use = C10_UNLIKELY(LU_new.has_value()) ? LU_new.value() : LU;

  // pivots
  set_contiguous_no_create(
      pivots, shape.slice(0, ndim - 1), A.options().dtype(kInt));

  // info is an aux tensor
  auto info = at::empty({0}, A.options().dtype(kInt));
  // Optimisation: lu_factor_ex requires the input to be F-contig, otherwise it
  // copies Use the transpose of if A is contiguous since det(A^T) = det(A) We
  // limit this to real matrices, but it could also be implemented for complex
  // matrices
  at::linalg_lu_factor_ex_out(
      const_cast<Tensor&>(LU_use),
      const_cast<Tensor&>(pivots),
      const_cast<Tensor&>(info),
      A.is_contiguous() && !A.is_complex() ? A.mH() : A);

  // det = det_P * prod(diag(LU))
  at::mul_out(
      const_cast<Tensor&>(det_use),
      lu_det_P(pivots),
      at::prod(LU_use.diagonal(0, -2, -1), /*dim=*/-1));
  if (det_new.has_value())
    det.copy_(det_use);
  if (LU_new.has_value())
    LU.copy_(LU_use);
  return std::tuple<Tensor&, Tensor&, Tensor&>(det, LU, pivots);
}

std::tuple<Tensor&, Tensor&, Tensor&, Tensor&> _linalg_slogdet_out(
    const Tensor& A,
    Tensor& sign,
    Tensor& logabsdet,
    Tensor& LU,
    Tensor& pivots) {
  at::native::squareCheckInputs(A, "linalg.slogdet");
  at::native::checkFloatingOrComplex(
      A, "linalg.slogdet", /*low_precision*/ false);
  auto shape = A.sizes();
  auto ndim = shape.size();

  auto shape_outputs = shape.slice(0, ndim - 2);

  // sign
  auto sign_new = set_contiguous(sign, shape_outputs, A.options());
  Tensor sign_use =
      C10_UNLIKELY(sign_new.has_value()) ? sign_new.value() : sign;

  // logabsdet
  auto logabsdet_new = set_contiguous(
      logabsdet,
      shape_outputs,
      A.options().dtype(toRealValueType(A.scalar_type())));
  Tensor logabsdet_use = C10_UNLIKELY(logabsdet_new.has_value())
      ? logabsdet_new.value()
      : logabsdet;

  // LU
  auto LU_strides = at::native::batched_matrix_contiguous_strides(
      shape,
      /*f-contig*=*/true);
  auto LU_new = set_strided(LU, shape, LU_strides, A.options());
  Tensor LU_use = C10_UNLIKELY(LU_new.has_value()) ? LU_new.value() : LU;

  // pivots
  set_contiguous_no_create(
      pivots, shape.slice(0, ndim - 1), A.options().dtype(kInt));

  // info is an aux tensor
  auto info = at::empty({0}, A.options().dtype(kInt));
  // Optimisation: lu_factor_ex requires the input to be F-contig, otherwise it
  // copies Use the transpose of if A is contiguous since det(A^T) = det(A) We
  // limit this to real matrices, but it could also be implemented for complex
  // matrices
  at::linalg_lu_factor_ex_out(
      const_cast<Tensor&>(LU_use),
      const_cast<Tensor&>(pivots),
      const_cast<Tensor&>(info),
      A.is_contiguous() && !A.is_complex() ? A.mH() : A);

  auto diag_U = LU_use.diagonal(0, -2, -1);
  // sign
  at::mul_out(
      const_cast<Tensor&>(sign_use), diag_U.sgn().prod(-1), lu_det_P(pivots));

  // logabsdet
  at::sum_out(const_cast<Tensor&>(logabsdet_use), diag_U.abs().log_(), -1);

  if (sign_new.has_value())
    sign.copy_(sign_use);
  if (logabsdet_new.has_value())
    logabsdet.copy_(logabsdet_use);
  if (LU_new.has_value())
    LU.copy_(LU_use);

  return std::tuple<Tensor&, Tensor&, Tensor&, Tensor&>(
      sign, logabsdet, LU, pivots);
}

std::tuple<Tensor&, Tensor&, Tensor&, Tensor&> _linalg_solve_ex_out(
    const Tensor& A,
    const Tensor& B,
    bool left,
    bool check_errors,
    Tensor& result,
    Tensor& LU,
    Tensor& pivots,
    Tensor& info) {
  TORCH_CHECK(
      A.scalar_type() == B.scalar_type(),
      "linalg.solve: Expected A and B to have the same dtype, but found A of type ",
      A.scalar_type(),
      " and B of type ",
      B.scalar_type(),
      " instead");

  // NumPy compat: Two types of 'B' tensors are supported:
  // - 1D tensor or batch of 1D tensors (vector case)
  // - 2D tensor or batch of 2D tensors (matrix case)
  const bool vector_case = at::native::linalg_solve_is_vector_rhs(A, B);
  auto B_ = vector_case ? B.unsqueeze(-1) : B;

  // matrix shapes
  at::native::checkInputsSolver(A, B_, /*left=*/left, "linalg.solve");

  // Check that B can be broadcasted to the shape of A
  auto B_broad_shape =
      std::get<0>(at::native::_linalg_broadcast_batch_dims(B_, A));
  // We disallow the broadcasting of B as a vector when left=False as, in that
  // case, A.shape = (*, 1, 1)
  TORCH_CHECK(
      left || !vector_case,
      "linalg.solve: Vector broadcasting of the left hand side is not supported for left=False. In this case linalg.solve is equivalent to B / A.squeeze(-1)");
  auto result_shape = vector_case
      ? IntArrayRef(B_broad_shape.data(), B_broad_shape.size() - 1)
      : B_broad_shape;
  auto result_strides = at::native::batched_matrix_contiguous_strides(
      result_shape, /*column_major=*/left);

  result.resize_(result_shape);
  result.as_strided_(result_shape, result_strides);
  auto shape = A.sizes();
  auto ndim = shape.size();

  // LU
  auto LU_strides =
      at::native::batched_matrix_contiguous_strides(shape, /*f-contig*=*/true);
  auto LU_new = set_strided(LU, shape, LU_strides, A.options());
  Tensor LU_use = C10_UNLIKELY(LU_new.has_value()) ? LU_new.value() : LU;

  // pivots
  set_contiguous_no_create(
      pivots, shape.slice(0, ndim - 1), A.options().dtype(kInt));

  // info
  set_contiguous_no_create(
      info, shape.slice(0, ndim - 2), A.options().dtype(kInt));

  const bool use_A_T = A.is_contiguous() && !A.is_complex();
  at::linalg_lu_factor_ex_out(
      const_cast<Tensor&>(LU_use),
      const_cast<Tensor&>(pivots),
      const_cast<Tensor&>(info),
      use_A_T ? A.mT() : A);
  if (check_errors) {
    at::_linalg_check_errors(info, "torch.linalg.solve_ex", A.dim() == 2);
  }

  // [numpy-compat] Handle vectors on the rhs
  const bool vector_case_B = at::native::linalg_solve_is_vector_rhs(LU_use, B);
  auto result_ = vector_case_B ? result.unsqueeze(-1) : result;
  at::linalg_lu_solve_out(
      result_, LU_use, pivots, B_, left, /*adjoint*/ use_A_T);
  if (LU_new.has_value())
    LU.copy_(LU_use);
  return std::tuple<Tensor&, Tensor&, Tensor&, Tensor&>(
      result, LU, pivots, info);
}

std::tuple<Tensor&, Tensor&, Tensor&> linalg_lu_factor_ex_out(
    const Tensor& A,
    bool pivot,
    bool check_errors,
    Tensor& LU,
    Tensor& pivots,
    Tensor& info) {
  TORCH_CHECK(
      A.dim() >= 2,
      "torch.lu_factor: Expected tensor with 2 or more dimensions. Got size: ",
      A.sizes(),
      " instead");

  auto sizes = A.sizes().vec();
  const auto m = sizes.cend()[-2];
  const auto n = sizes.cend()[-1];

  // make column major strides for BLAS
  auto LU_strides = at::native::batched_matrix_contiguous_strides(
      sizes,
      /*f-contig*=*/true);
  auto LU_new = set_strided(LU, sizes, LU_strides, A.options());
  Tensor LU_use = C10_UNLIKELY(LU_new.has_value()) ? LU_new.value() : LU;

  // Set sizes to the size of pivots
  sizes.pop_back();
  sizes.back() = std::min(m, n);
  set_contiguous_no_create(pivots, sizes, A.options().dtype(kInt));

  // Set sizes to the size of info
  sizes.pop_back();
  set_contiguous_no_create(info, sizes, A.options().dtype(kInt));

  TORCH_CHECK(
      pivot,
      "linalg.lu_factor: LU without pivoting is not implemented on the XPU");
  if (A.numel() == 0) {
    // zero out the infos as it will have one element if the input is a matrix
    // of size (0, 0)
    info.zero_();
    return std::tuple<Tensor&, Tensor&, Tensor&>(LU, pivots, info);
  }

  if (!LU_use.is_same(A)) {
    LU_use.copy_(A);
  }
  // handle the info
  std::vector<int32_t> infos_vec(native::batchCount(A), 0);
  // mkl needs long for pivots, but PT is int
  Tensor pivots_ = at::empty(pivots.sizes(), pivots.options().dtype(kLong));
  IPEX_DISPATCH_FLOATING_AND_COMPLEX_TYPES(
      LU_use.scalar_type(), "lu_dpcpp", [&] {
        impl::apply_lu_dpcpp_<scalar_t>(LU_use, pivots_, infos_vec);
      });
  auto expected_info_shape =
      IntArrayRef(LU_use.sizes().cbegin(), LU_use.sizes().cend() - 2);

  info.copy_(at::from_blob(
      (int32_t*)(infos_vec.data()),
      expected_info_shape,
      c10::toRealValueType(info.scalar_type())));

  if (check_errors) {
    at::_linalg_check_errors(info, "torch.linalg.lu_factor_ex", A.dim() == 2);
  }
  // Copy to original pivots tensor
  pivots.copy_(pivots_);
  if (LU_new.has_value())
    LU.copy_(LU_use);
  return std::tuple<Tensor&, Tensor&, Tensor&>(LU, pivots, info);
}

} // namespace AtenIpexTypeXPU
} // namespace at
