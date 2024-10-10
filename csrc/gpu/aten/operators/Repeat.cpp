#include <ATen/ATen.h>
#include <ATen/native/Repeat.h>

#include <core/Memory.h>
#include <runtime/Utils.h>
#include <utils/Helpers.h>
#include "comm/ATDispatch.h"
#include "comm/RegistrationDeclarations.h"

using namespace at::native;
using namespace torch_ipex::xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

template <typename index_t>
struct RepeatInterleaveDpcppKernelFunctor {
  void operator()(sycl::nd_item<1> item) const {
    auto rep_ptr = rep_data;
    auto cum_ptr = cum_data;
    auto res_ptr = res_data;

    for (int64_t i = item.get_global_id(0); i < size;
         i += item.get_global_range()[0]) {
      int64_t end = cum_ptr[i];
      int64_t repeat = rep_ptr[i];
      int64_t start = end - repeat;
      for (int64_t j = start; j < end; j++) {
        res_ptr[j] = i;
      }
    }
  }
  RepeatInterleaveDpcppKernelFunctor(
      const index_t* rep_data_,
      const int64_t* cum_data_,
      index_t* res_data_,
      int64_t size_,
      int64_t result_size_)
      : rep_data(rep_data_),
        cum_data(cum_data_),
        res_data(res_data_),
        size(size_),
        result_size(result_size_) {}

 private:
  const index_t* rep_data;
  const int64_t* cum_data;
  index_t* res_data;
  int64_t size;
  int64_t result_size;
};

template <typename index_t>
static void repeat_interleave_dpcpp_kernel(
    const index_t* repeat_ptr,
    const int64_t* cumsum_ptr,
    index_t* result_ptr,
    int64_t size,
    int64_t result_size) {
  auto& queue = dpcppGetCurrentQueue();
  int64_t local_range = static_cast<int64_t>(1);
  int64_t global_range = static_cast<int64_t>(1);
  if (size != 0) {
    int64_t wg_size = dpcppMaxWorkGroupSize();
    local_range = size < wg_size ? size : wg_size;
    global_range = ((size + local_range - 1) / local_range) * local_range;
  }

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto rep_data = repeat_ptr;
    auto cum_data = cumsum_ptr;
    auto res_data = result_ptr;

    RepeatInterleaveDpcppKernelFunctor<index_t> kfn(
        rep_data, cum_data, res_data, size, result_size);
    // kick off kernel
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<1>(
            sycl::range<1>(global_range), sycl::range<1>(local_range)),
        kfn);
  };

  DPCPP_Q_SUBMIT(queue, cgf);
}

// static void repeat_interleave_dpcpp(int64_t *repeat_ptr, int64_t *cumsum_ptr,
// int64_t *result_ptr, int64_t size) {
//   repeat_interleave_dpcpp_kernel(repeat_ptr, cumsum_ptr, result_ptr, size);
// }

} // namespace impl

Tensor repeat_interleave(
    const Tensor& repeat,
    c10::optional<int64_t> output_size) {
  Tensor output;
  IPEX_DISPATCH_INDEX_TYPES(repeat.scalar_type(), "repeat_interleave", [&] {
    output = repeat_interleave_common<
        index_t,
        impl::repeat_interleave_dpcpp_kernel<index_t>>(repeat, output_size);
  });
  return output;
}

Tensor _reshape_alias(
    const Tensor& self,
    IntArrayRef size,
    IntArrayRef stride) {
  return at::native::_reshape_alias(self, size, stride);
}

} // namespace AtenIpexTypeXPU

namespace AtenIpexTypeQuantizedXPU {
Tensor _reshape_alias(
    const Tensor& self,
    IntArrayRef size,
    IntArrayRef stride) {
  return at::native::_reshape_alias(self, size, stride);
}
} // namespace AtenIpexTypeQuantizedXPU

} // namespace at
