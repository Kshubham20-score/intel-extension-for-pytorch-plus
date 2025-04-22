#include <ATen/ATen.h>

namespace at {

namespace AtenIpexTypeQuantizedXPU {

Tensor clone(const Tensor& self, c10::optional<MemoryFormat> memory_format) {
  // We should not call the quantized_clone for QuantizedXPU.
  // The quantized_clone only supports QuantizedCPU and QuantizedCUDA.
  // We should call aten clone directly, while the quantization
  // case will be handled by our copy impl.
  return at::native::clone(self, memory_format);
}

} // namespace AtenIpexTypeQuantizedXPU
} // namespace at
