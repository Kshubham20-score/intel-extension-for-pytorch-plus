#include <ATen/ATen.h>
#include <ATen/DeviceGuard.h>
#include <ATen/Functions.h>
#include <ATen/ceil_div.h>
#include <ATen/native/Activation.h>
#include <ATen/record_function.h>
#include <core/Memory.h>
#include <runtime/Utils.h>
#include <tensor/Tensor.h>
#include <utils/DPCPP.h>
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/Numerics.h"
#include "utils/CustomOperatorRegistration.h"

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

int64_t upper_power_of_two(int64_t v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v++;
  return v;
}

template <typename scalar_t, typename accscalar_t>
inline accscalar_t zero_norm(scalar_t data) {
  return data == static_cast<scalar_t>(0) ? accscalar_t(0) : accscalar_t(1);
}

template <typename scalar_t, typename accscalar_t>
inline accscalar_t one_norm(scalar_t data) {
  return static_cast<accscalar_t>(Numerics<scalar_t>::fabs(data));
}

template <typename scalar_t, typename accscalar_t>
inline accscalar_t p_norm(scalar_t data, scalar_t norm) {
  return Numerics<accscalar_t>::pow(
      static_cast<accscalar_t>(Numerics<scalar_t>::fabs(data)), norm);
}

template <typename scalar_t, typename accscalar_t>
struct NormClampMulFusionKernelFunctor1 {
  void operator()(sycl::nd_item<2> item) const {
    int tensor_index = item.get_global_id()[1];
    int local_id = item.get_local_id()[0];
    int local_range = item.get_local_range()[0];

    int cur_tensor_size = size_storage_data[tensor_index];
    scalar_t* grad_ptr_storage = grad_storage_data[tensor_index];
    scalar_t* norm_ptr_storage = norm_storage_data[tensor_index];

    auto loop_times = ceil_div<int>(cur_tensor_size, local_range);

    // step1, each work_group will calculate the norm for a tensor
    for (int i = 0; i < loop_times; i++) {
      int numel_index = i * local_range + local_id;
      if (numel_index < cur_tensor_size) {
        if (norm_type == 0.0f) {
          norm_ptr_storage[local_id] += zero_norm<scalar_t, accscalar_t>(
              static_cast<accscalar_t>(grad_ptr_storage[numel_index]));
        } else if (norm_type == 1.0f) {
          norm_ptr_storage[local_id] += one_norm<scalar_t, accscalar_t>(
              static_cast<accscalar_t>(grad_ptr_storage[numel_index]));
        } else {
          norm_ptr_storage[local_id] += p_norm<scalar_t, accscalar_t>(
              static_cast<accscalar_t>(grad_ptr_storage[numel_index]),
              norm_type);
        }
      }
    }

    item.barrier(dpcpp_global_fence);

    // step2, each work_group reduce a sum for a tensor
    auto reduce_size = Numerics<int>::min(cur_tensor_size, local_range);
    int64_t cal_size = upper_power_of_two(reduce_size);

    for (int i = cal_size / 2; i >= 1; i /= 2) {
      if (local_id < i && local_id + i < reduce_size) {
        norm_ptr_storage[local_id] += norm_ptr_storage[local_id + i];
      }
      item.barrier(dpcpp_global_fence);
    }

    if (local_id == 0) {
      if (norm_type != 0.0 && norm_type != 1.0) {
        norm_result_addr[tensor_index] = p_norm<scalar_t, accscalar_t>(
            norm_ptr_storage[local_id],
            static_cast<accscalar_t>(1.0f / norm_type));
      } else {
        norm_result_addr[tensor_index] = norm_ptr_storage[local_id];
      }
    }
  }
  NormClampMulFusionKernelFunctor1(
      int64_t local_range_,
      int* size_storage_data_,
      scalar_t** grad_storage_data_,
      scalar_t** norm_storage_data_,
      scalar_t* norm_result_addr_,
      scalar_t* return_norm_addr_,
      double norm_type_)
      : local_range(local_range_),
        size_storage_data(size_storage_data_),
        grad_storage_data(grad_storage_data_),
        norm_storage_data(norm_storage_data_),
        norm_result_addr(norm_result_addr_),
        return_norm_addr(return_norm_addr_),
        norm_type(norm_type_) {}

 private:
  int64_t local_range;
  int* size_storage_data;
  scalar_t** grad_storage_data;
  scalar_t** norm_storage_data;
  scalar_t* norm_result_addr;
  scalar_t* return_norm_addr;
  double norm_type;
};

template <typename scalar_t, typename accscalar_t>
struct NormClampMulFusionKernelFunctor2 {
  void operator()(sycl::nd_item<1> item) const {
    int local_id = item.get_global_linear_id();

    // step3, calculate the whole norm result for norm vector
    if (local_id < tensor_size) {
      if (norm_type == 0.0f) {
        norm_result_addr[local_id] =
            zero_norm<scalar_t, accscalar_t>(norm_result_addr[local_id]);
      } else if (norm_type == 1.0f) {
        norm_result_addr[local_id] =
            one_norm<scalar_t, accscalar_t>(norm_result_addr[local_id]);
      } else {
        norm_result_addr[local_id] = p_norm<scalar_t, accscalar_t>(
            norm_result_addr[local_id], norm_type);
      }
    }
    item.barrier(dpcpp_global_fence);

    // step4, calculate the whole norm result for norm vector
    auto tensor_reduce_size = tensor_size;

    for (int i = tensor_reduce_size / 2; i >= 1; i /= 2) {
      if (local_id < i && local_id + i < tensor_reduce_size) {
        norm_result_addr[local_id] += norm_result_addr[local_id + i];
      }
      item.barrier(dpcpp_global_fence);
    }

    if (local_id == 0) {
      if (norm_type != 0.0 && norm_type != 1.0) {
        norm_result_addr[0] = p_norm<scalar_t, accscalar_t>(
            norm_result_addr[local_id],
            static_cast<accscalar_t>(1.0f / norm_type));
      }
      return_norm_addr[0] = static_cast<double>(norm_result_addr[0]);
    }
    item.barrier(dpcpp_global_fence);
    SYCL_KERNEL_ASSERT(
        !(error_if_nonfinite &&
          (Numerics<accscalar_t>::isnan(norm_result_addr[0]) ||
           Numerics<accscalar_t>::isinf(norm_result_addr[0]))) &&
        "The total norm of gradients from parameters is non-finite, so it cannot be clipped. To disable this error and scale the gradients by the non-finite norm anyway, set error_if_nonfinite=False");
  }
  NormClampMulFusionKernelFunctor2(
      scalar_t* norm_result_addr_,
      scalar_t* return_norm_addr_,
      double norm_type_,
      int tensor_size_,
      bool error_if_nonfinite_)
      : norm_result_addr(norm_result_addr_),
        return_norm_addr(return_norm_addr_),
        norm_type(norm_type_),
        tensor_size(tensor_size_),
        error_if_nonfinite(error_if_nonfinite_) {}

 private:
  scalar_t* norm_result_addr;
  scalar_t* return_norm_addr;
  double norm_type;
  int tensor_size;
  bool error_if_nonfinite;
};

template <typename scalar_t, typename accscalar_t>
struct NormClampMulFusionKernelFunctor3 {
  void operator()(sycl::nd_item<2> item) const {
    int tensor_index = item.get_global_id()[1];
    int local_id = item.get_local_id()[0];
    int local_range = item.get_local_range()[0];

    int cur_tensor_size = size_storage_data[tensor_index];
    scalar_t* grad_ptr_storage = grad_storage_data[tensor_index];
    scalar_t* norm_ptr_storage = norm_storage_data[tensor_index];

    auto loop_times = ceil_div<int>(tensor_size, local_range);

    // step 5 calculate clip_coef and update grads
    auto total_norm = norm_result_addr[0];
    auto clip_coef = max_norm / (total_norm + 1e-6);
    auto clip_coef_clamped = clip_coef > 1.0f ? 1.0f : clip_coef;

    for (int i = 0; i < loop_times; i++) {
      int numel_index = i * local_range + local_id;
      if (numel_index < cur_tensor_size) {
        grad_ptr_storage[numel_index] *= clip_coef_clamped;
      }
    }
  }
  NormClampMulFusionKernelFunctor3(
      int* size_storage_data_,
      scalar_t** grad_storage_data_,
      scalar_t** norm_storage_data_,
      scalar_t* norm_result_addr_,
      int tensor_size_,
      double max_norm_)
      : size_storage_data(size_storage_data_),
        grad_storage_data(grad_storage_data_),
        norm_storage_data(norm_storage_data_),
        norm_result_addr(norm_result_addr_),
        tensor_size(tensor_size_),
        max_norm(max_norm_) {}

 private:
  int* size_storage_data;
  scalar_t** grad_storage_data;
  scalar_t** norm_storage_data;
  scalar_t* norm_result_addr;
  int tensor_size;
  double max_norm;
};

template <typename scalar_t, typename accscalar_t>
void norm_clamp_mul_fusion(
    std::vector<Tensor> grads,
    std::vector<Tensor> norm_result_temp,
    Tensor norm_tensor,
    double max_norm,
    double norm_type,
    Tensor return_norm,
    int64_t max_tensor_numel,
    bool error_if_nonfinite) {
  auto maxWorkGroupSize = dpcppMaxWorkGroupSize();
  int tensor_size = grads.size();
  auto local_range =
      max_tensor_numel > maxWorkGroupSize ? maxWorkGroupSize : max_tensor_numel;
  auto global_range = ceil_div<int>(tensor_size, local_range);

  // used to store each grad address for tensor
  auto grad_ptr_storage = at::empty(
      {grads.size() * sizeof(scalar_t*)},
      grads[0].options().dtype(at::kByte).device(at::kCPU));

  // used to store each grad size for tensor
  auto size_storage = at::empty(
      {grads.size()}, grads[0].options().dtype(at::kInt).device(at::kCPU));

  // used to store each grad norm result for tensor
  auto norm_ptr_storage = at::empty(
      {norm_result_temp.size() * sizeof(scalar_t*)},
      norm_result_temp[0].options().dtype(at::kByte).device(at::kCPU));

  int* size_storage_addr = size_storage.data_ptr<int>();
  scalar_t** ptr_storage_addr =
      static_cast<scalar_t**>(grad_ptr_storage.data_ptr());
  scalar_t** norm_storage_addr =
      static_cast<scalar_t**>(norm_ptr_storage.data_ptr());

  for (int i = 0; i < grads.size(); i++) {
    auto t = grads[i];
    auto n = norm_result_temp[i];
    size_storage_addr[i] = t.numel();
    ptr_storage_addr[i] = t.data_ptr<scalar_t>();
    norm_storage_addr[i] = n.data_ptr<scalar_t>();
  }

  size_storage = size_storage.to(at::kXPU);
  grad_ptr_storage = grad_ptr_storage.to(at::kXPU);
  norm_ptr_storage = norm_ptr_storage.to(at::kXPU);

  int* size_storage_data = size_storage.data_ptr<int>();
  scalar_t** grad_storage_data =
      static_cast<scalar_t**>(grad_ptr_storage.data_ptr());
  scalar_t** norm_storage_data =
      static_cast<scalar_t**>(norm_ptr_storage.data_ptr());
  scalar_t* norm_result_addr = static_cast<scalar_t*>(norm_tensor.data_ptr());
  scalar_t* return_norm_addr = static_cast<scalar_t*>(return_norm.data_ptr());

  auto cgf_1 = DPCPP_Q_CGF(cgh) {
    NormClampMulFusionKernelFunctor1<scalar_t, accscalar_t> kfn_1(
        local_range,
        size_storage_data,
        grad_storage_data,
        norm_storage_data,
        norm_result_addr,
        return_norm_addr,
        norm_type);
    cgh.parallel_for<decltype(kfn_1)>(
        sycl::nd_range<2>(
            sycl::range<2>(local_range, tensor_size),
            sycl::range<2>(local_range, 1)),
        kfn_1);
  };

  DPCPP_Q_SUBMIT(dpcppGetCurrentQueue(), cgf_1);

  auto cgf_2 = DPCPP_Q_CGF(cgh) {
    NormClampMulFusionKernelFunctor2<scalar_t, accscalar_t> kfn_2(
        norm_result_addr,
        return_norm_addr,
        norm_type,
        tensor_size,
        error_if_nonfinite);
    cgh.parallel_for<decltype(kfn_2)>(
        sycl::nd_range<1>(global_range * local_range, local_range), kfn_2);
  };

  DPCPP_Q_SUBMIT(dpcppGetCurrentQueue(), cgf_2);

  auto cgf_3 = DPCPP_Q_CGF(cgh) {
    NormClampMulFusionKernelFunctor3<scalar_t, accscalar_t> kfn_3(
        size_storage_data,
        grad_storage_data,
        norm_storage_data,
        norm_result_addr,
        tensor_size,
        max_norm);
    cgh.parallel_for<decltype(kfn_3)>(
        sycl::nd_range<2>(
            sycl::range<2>(local_range, tensor_size),
            sycl::range<2>(local_range, 1)),
        kfn_3);
  };

  DPCPP_Q_SUBMIT(dpcppGetCurrentQueue(), cgf_3);
  return;
}

} // namespace impl

// Return the calculated norm for whole tensorlist, and update tensorlist
// according to the norm value
//
// input:
//   grads: grads needs update
//   max_norm: max_norm_value, ceiling value for grad_norm
//   norm_type: calcluate norm_type, currently support zero_norm p_norm one_norm
//   error_if_nonfinite: whether it is error when getting infinitly or nan norm

// output:
//   calculated norm the whole tensor list
at::Tensor fused_clip_grad_norm(
    std::vector<Tensor> grads,
    double max_norm,
    double norm_type,
    bool is_inf,
    bool error_if_nonfinite) {
  double total_norm = 0.0f;

  // used for store the norm result, calculated from grads
  std::vector<Tensor> norm_result_temp;
  int64_t max_tensor_numel = 0;
  for (int i = 0; i < grads.size(); i++) {
    norm_result_temp.push_back(at::zeros_like(grads[i]));
    max_tensor_numel =
        Numerics<int64_t>::max(grads[i].numel(), max_tensor_numel);
  }

  auto norm_tensor =
      at::empty({grads.size()}, grads[0].options().dtype(at::kDouble));

  auto return_norm = at::empty({}, grads[0].options());

  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      kHalf, kBFloat16, grads[0].scalar_type(), "fused_clip_grad_norm", [&] {
        using accscalar_t = acc_type<scalar_t>;
        impl::norm_clamp_mul_fusion<scalar_t, accscalar_t>(
            grads,
            norm_result_temp,
            norm_tensor,
            max_norm,
            norm_type,
            return_norm,
            max_tensor_numel,
            error_if_nonfinite);
      });
  // return_norm = return_norm.to(at::kFloat);

  return return_norm;
}

} // namespace AtenIpexTypeXPU
} // namespace at

namespace {
IPEX_LIBRARY_FRAGMENT() {
  IPEX_OP_REGISTER_DISPATCH(
      "fused_clip_grad_norm",
      at::AtenIpexTypeXPU::fused_clip_grad_norm,
      c10::DispatchKey::XPU);
}
} // namespace
