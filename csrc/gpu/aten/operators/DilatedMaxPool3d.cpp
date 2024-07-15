#include <ATen/ATen.h>
#include <ATen/Config.h>
#include <ATen/NativeFunctions.h>
#include <ATen/native/Pool.h>

#include <oneDNN/oneDNN.h>
#include "comm/ATDispatch.h"
#include "comm/Atomics.h"
#include "comm/RegistrationDeclarations.h"
#include "utils/ComputeEngine.h"

#include <vector>

using namespace dnnl;
using namespace torch_ipex::xpu::dpcpp;
using namespace at::native;
using namespace torch_ipex::xpu::oneDNN;

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

template <typename scalar_t, bool channels_last>
struct MaxPool3dWithIndicesOutFrameImplKernelFunctor {
  void operator()(sycl::nd_item<1> item) const {
    for (auto outputIndex = item.get_global_id(0); outputIndex < OutputSize;
         outputIndex += global_range) {
      int batch = 0;
      int channel = 0;
      int oDepth = 0;
      int oRow = 0;
      int oColumn = 0;
      // used only for channels-first indexing
      int64_t slice = 0;
      batch = outputIndex / out_batch_stride;
      if constexpr (!channels_last) {
        // indexing order: batch, channel, depth
        oColumn = outputIndex % OutputSizeW;
        oRow = outputIndex / OutputSizeW % OutputSizeH;
        oDepth = outputIndex / out_cf_d_stride % OutputSizeD;
        channel = outputIndex / out_cf_c_stride % numChannels;
        slice = outputIndex / out_cf_c_stride;
      } else {
        channel = outputIndex % numChannels;
        oColumn = outputIndex / numChannels % OutputSizeW;
        oRow = outputIndex / out_cl_h_stride % OutputSizeH;
        oDepth = outputIndex / out_cl_d_stride % OutputSizeD;
        slice = outputIndex / out_cf_d_stride;
      }

      // For int64_t data type, see
      // https://github.com/pytorch/pytorch/issues/52822
      int tStart = oDepth * dT - pT;
      int hStart = oRow * dH - pH;
      int wStart = oColumn * dW - pW;
      int tEnd = std::min(tStart + (kT - 1) * dilationT + 1, InputSizeD);
      int hEnd = std::min(hStart + (kH - 1) * dilationH + 1, InputSizeH);
      int wEnd = std::min(wStart + (kW - 1) * dilationW + 1, InputSizeW);

      while (tStart < 0)
        tStart += dilationT;
      while (hStart < 0)
        hStart += dilationH;
      while (wStart < 0)
        wStart += dilationW;

      int64_t maxIndex;
      int64_t ioffset;

      if constexpr (!channels_last) {
        ioffset = (int64_t)slice * in_cf_c_stride;
      } else {
        ioffset = ((int64_t)batch * in_batch_stride) + channel;
      }

      scalar_t max = Numerics<scalar_t>::lower_bound(); // -Infinity

      for (int t = tStart; t < tEnd; t += dilationT) {
        for (int h = hStart; h < hEnd; h += dilationH) {
          for (int w = wStart; w < wEnd; w += dilationW) {
            scalar_t val;
            int index = t * in_hw_stride + h * InputSizeW + w;
            if constexpr (!channels_last) {
              val = inputData[ioffset + index];
            } else {
              int64_t index_channels_last = index * numChannels;
              val = inputData[ioffset + index_channels_last];
            }

            if ((max < val) || std::isnan(val)) {
              max = val;
              maxIndex = index;
            }
          }
        }
      }

      int64_t out_index;
      if constexpr (!channels_last) {
        out_index = (int64_t)slice * out_cf_c_stride +
            (int64_t)oDepth * out_cf_d_stride + (int64_t)oRow * OutputSizeW +
            oColumn;
      } else {
        out_index = (int64_t)batch * out_batch_stride +
            (int64_t)oDepth * out_cl_d_stride +
            (int64_t)oRow * out_cl_h_stride + (int64_t)oColumn * numChannels +
            channel;
      }
      outputData[out_index] = max;
      indicesData[out_index] = maxIndex;
    }
  }

  MaxPool3dWithIndicesOutFrameImplKernelFunctor(
      scalar_t* inputData_,
      scalar_t* outputData_,
      int64_t* indicesData_,
      int numChannels_,
      int InputSizeD_,
      int InputSizeH_,
      int InputSizeW_,
      int nbatch_,
      int OutputSizeD_,
      int OutputSizeH_,
      int OutputSizeW_,
      int kT_,
      int kH_,
      int kW_,
      int dT_,
      int dH_,
      int dW_,
      int pT_,
      int pH_,
      int pW_,
      int dilationT_,
      int dilationH_,
      int dilationW_,
      int64_t wg_size_,
      int64_t OutputSize_,
      int global_range_,
      int out_cf_d_stride_,
      int out_cf_c_stride_,
      int in_cf_d_stride_,
      int in_cf_c_stride_,
      int out_cl_h_stride_,
      int out_cl_d_stride_,
      int in_cl_h_stride_,
      int in_cl_d_stride_,
      int in_batch_stride_,
      int out_batch_stride_,
      int in_hw_stride_)
      : inputData(inputData_),
        outputData(outputData_),
        indicesData(indicesData_),
        numChannels(numChannels_),
        InputSizeD(InputSizeD_),
        InputSizeH(InputSizeH_),
        InputSizeW(InputSizeW_),
        nbatch(nbatch_),
        OutputSizeD(OutputSizeD_),
        OutputSizeH(OutputSizeH_),
        OutputSizeW(OutputSizeW_),
        kT(kT_),
        kH(kH_),
        kW(kW_),
        dT(dT_),
        dH(dH_),
        dW(dW_),
        pT(pT_),
        pH(pH_),
        pW(pW_),
        dilationT(dilationT_),
        dilationH(dilationH_),
        dilationW(dilationW_),
        wg_size(wg_size_),
        OutputSize(OutputSize_),
        global_range(global_range_),
        out_cf_d_stride(out_cf_d_stride_),
        out_cf_c_stride(out_cf_c_stride_),
        in_cf_d_stride(in_cf_d_stride_),
        in_cf_c_stride(in_cf_c_stride_),
        out_cl_h_stride(out_cl_h_stride_),
        out_cl_d_stride(out_cl_d_stride_),
        in_cl_h_stride(in_cl_h_stride_),
        in_cl_d_stride(in_cl_d_stride_),
        in_batch_stride(in_batch_stride_),
        out_batch_stride(out_batch_stride_),
        in_hw_stride(in_hw_stride_) {}

 private:
  scalar_t* inputData;
  scalar_t* outputData;
  int64_t* indicesData;
  int numChannels;
  int InputSizeD;
  int InputSizeH;
  int InputSizeW;
  int nbatch;
  int OutputSizeD;
  int OutputSizeH;
  int OutputSizeW;
  int kT;
  int kH;
  int kW;
  int dT;
  int dH;
  int dW;
  int pT;
  int pH;
  int pW;
  int dilationT;
  int dilationH;
  int dilationW;
  int64_t wg_size;
  int64_t OutputSize;
  int global_range;
  int out_cf_d_stride;
  int out_cf_c_stride;
  int in_cf_d_stride;
  int in_cf_c_stride;
  int out_cl_h_stride;
  int out_cl_d_stride;
  int in_cl_h_stride;
  int in_cl_d_stride;
  int in_batch_stride;
  int out_batch_stride;
  int in_hw_stride;
};

template <typename scalar_t, bool channels_last>
static void max_pool3d_with_indices_out_frame_impl(
    scalar_t* inputData,
    scalar_t* outputData,
    int64_t* indicesData,
    int numChannels,
    int InputSizeD,
    int InputSizeH,
    int InputSizeW,
    int nbatch,
    int OutputSizeD,
    int OutputSizeH,
    int OutputSizeW,
    int kT,
    int kH,
    int kW,
    int dT,
    int dH,
    int dW,
    int pT,
    int pH,
    int pW,
    int dilationT,
    int dilationH,
    int dilationW) {
  auto& queue = dpcppGetCurrentQueue();
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  int64_t wg_size = dpcppMaxWorkItemsPerEU(dev_id);
  int64_t OutputSize =
      nbatch * numChannels * OutputSizeD * OutputSizeH * OutputSizeW;
  int work_group_size = OutputSize > wg_size ? wg_size : OutputSize;
  auto global_range =
      (OutputSize + work_group_size - 1) / work_group_size * work_group_size;
  global_range = std::min(
      global_range,
      dpcppMaxWorkItemsPerTile(dev_id) / work_group_size * work_group_size);
  int out_cf_d_stride, out_cf_c_stride, in_cf_d_stride, in_cf_c_stride;
  int out_cl_h_stride, out_cl_d_stride, in_cl_h_stride, in_cl_d_stride;
  if constexpr (!channels_last) {
    out_cf_d_stride = OutputSizeW * OutputSizeH;
    out_cf_c_stride = OutputSizeD * out_cf_d_stride;
    in_cf_d_stride = InputSizeW * InputSizeH;
    in_cf_c_stride = InputSizeD * in_cf_d_stride;
  } else {
    out_cl_h_stride = OutputSizeW * numChannels;
    out_cl_d_stride = OutputSizeH * out_cl_h_stride;
  }
  auto in_batch_stride = InputSizeD * InputSizeH * InputSizeW * numChannels;
  auto out_batch_stride = OutputSizeD * OutputSizeH * OutputSizeW * numChannels;
  auto in_hw_stride = InputSizeH * InputSizeW;
  auto cgf = DPCPP_Q_CGF(cgh) {
    MaxPool3dWithIndicesOutFrameImplKernelFunctor<scalar_t, channels_last> kfn(
        inputData,
        outputData,
        indicesData,
        numChannels,
        InputSizeD,
        InputSizeH,
        InputSizeW,
        nbatch,
        OutputSizeD,
        OutputSizeH,
        OutputSizeW,
        kT,
        kH,
        kW,
        dT,
        dH,
        dW,
        pT,
        pH,
        pW,
        dilationT,
        dilationH,
        dilationW,
        wg_size,
        OutputSize,
        global_range,
        out_cf_d_stride,
        out_cf_c_stride,
        in_cf_d_stride,
        in_cf_c_stride,
        out_cl_h_stride,
        out_cl_d_stride,
        in_cl_h_stride,
        in_cl_d_stride,
        in_batch_stride,
        out_batch_stride,
        in_hw_stride);
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<1>(
            sycl::range<1>(global_range), sycl::range<1>(work_group_size)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

template <typename scalar_t, bool channels_last>
struct MaxPool3dWithIndicesBackwardOutFrameImplKernelFunctor {
  void operator()(sycl::nd_item<1> item) const {
    for (auto outputIndex = item.get_global_id(0); outputIndex < gradOutputSize;
         outputIndex += global_range) {
      int batch = outputIndex / out_nbatch_stride;
      if constexpr (channels_last) {
        int channel = outputIndex % numChannels;
        int64_t index = indices_ptr[outputIndex];
        int64_t gradIn_offset =
            batch * in_nbatch_stride + channel + index * numChannels;
        atomicAdd(
            (dpcpp_global_ptr_pt<scalar_t>)&gradInput_ptr[gradIn_offset],
            gradOutput_ptr[outputIndex]);
      } else {
        int channel = outputIndex / out_cf_channel_stride % numChannels;
        int64_t index = indices_ptr[outputIndex];
        int64_t gradIn_offset =
            batch * in_nbatch_stride + channel * in_cf_channel_stride + index;
        atomicAdd(
            (dpcpp_global_ptr_pt<scalar_t>)&gradInput_ptr[gradIn_offset],
            gradOutput_ptr[outputIndex]);
      }
    }
  }
  MaxPool3dWithIndicesBackwardOutFrameImplKernelFunctor(
      scalar_t* gradInput_ptr_,
      scalar_t* gradOutput_ptr_,
      int64_t* indices_ptr_,
      int numChannels_,
      int64_t gradOutputSize_,
      int64_t global_range_,
      int out_cf_channel_stride_,
      int in_cf_channel_stride_,
      int out_nbatch_stride_,
      int in_nbatch_stride_)
      : gradInput_ptr(gradInput_ptr_),
        gradOutput_ptr(gradOutput_ptr_),
        indices_ptr(indices_ptr_),
        numChannels(numChannels_),
        gradOutputSize(gradOutputSize_),
        global_range(global_range_),
        out_cf_channel_stride(out_cf_channel_stride_),
        in_cf_channel_stride(in_cf_channel_stride_),
        out_nbatch_stride(out_nbatch_stride_),
        in_nbatch_stride(in_nbatch_stride_) {}

 private:
  scalar_t* gradInput_ptr;
  scalar_t* gradOutput_ptr;
  int64_t* indices_ptr;
  int numChannels;
  int64_t gradOutputSize;
  int64_t global_range;
  int out_cf_channel_stride;
  int in_cf_channel_stride;
  int out_nbatch_stride;
  int in_nbatch_stride;
};

template <typename scalar_t, bool channels_last>
static void max_pool3d_with_indices_backward_out_frame_impl(
    scalar_t* gradInput_ptr,
    scalar_t* gradOutput_ptr,
    int64_t* indices_ptr,
    int numChannels,
    int gradInputSizeD,
    int gradInputSizeH,
    int gradInputSizeW,
    int nbatch,
    int gradOutputSizeD,
    int gradOutputSizeH,
    int gradOutputSizeW) {
  auto& queue = dpcppGetCurrentQueue();
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  int64_t max_wg_size = dpcppMaxWorkItemsPerEU(dev_id);
  int64_t gradOutputSize = nbatch * numChannels * gradOutputSizeD *
      gradOutputSizeH * gradOutputSizeW;
  int work_group_size =
      gradOutputSize > max_wg_size ? max_wg_size : gradOutputSize;
  auto global_range =
      ((gradOutputSize - 1) / work_group_size + 1) * work_group_size;
  global_range = std::min(global_range, dpcppMaxWorkItemsPerTile(dev_id));
  auto out_cf_channel_stride =
      gradOutputSizeD * gradOutputSizeH * gradOutputSizeW;
  auto in_cf_channel_stride = gradInputSizeD * gradInputSizeH * gradInputSizeW;
  auto out_nbatch_stride = numChannels * out_cf_channel_stride;
  auto in_nbatch_stride = numChannels * in_cf_channel_stride;

  auto cgf = DPCPP_Q_CGF(cgh) {
    MaxPool3dWithIndicesBackwardOutFrameImplKernelFunctor<
        scalar_t,
        channels_last>
        kfn(gradInput_ptr,
            gradOutput_ptr,
            indices_ptr,
            numChannels,
            gradOutputSize,
            global_range,
            out_cf_channel_stride,
            in_cf_channel_stride,
            out_nbatch_stride,
            in_nbatch_stride);
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<1>(
            sycl::range<1>(global_range), sycl::range<1>(work_group_size)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

void max_pool3d_with_indices_out_template(
    Tensor& output,
    Tensor& indices,
    const Tensor& input,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    IntArrayRef dilation,
    bool ceil_mode) {
  if (input.numel() == 0) {
    return;
  }

  TORCH_CHECK(
      kernel_size.size() == 1 || kernel_size.size() == 3,
      "max_pool3d: kernel_size must either be a single int, or a tuple "
      "of three ints")
  const int kD = safe_downcast<int, int64_t>(kernel_size[0]);
  const int kH = kernel_size.size() == 1
      ? kD
      : safe_downcast<int, int64_t>(kernel_size[1]);
  const int kW = kernel_size.size() == 1
      ? kD
      : safe_downcast<int, int64_t>(kernel_size[2]);

  TORCH_CHECK(
      stride.size() == 0 || stride.size() == 1 || stride.size() == 3,
      "max_pool3d: stride must either be omitted, a single int, or a tuple of "
      "three ints")
  const int dD = stride.empty() ? kD : safe_downcast<int, int64_t>(stride[0]);
  const int dH = stride.empty() ? kH
      : stride.size() == 1      ? dD
                                : safe_downcast<int, int64_t>(stride[1]);
  const int dW = stride.empty() ? kW
      : stride.size() == 1      ? dD
                                : safe_downcast<int, int64_t>(stride[2]);

  TORCH_CHECK(
      padding.size() == 1 || padding.size() == 3,
      "max_pool3d: padding must be either be a single int, or a tuple of three "
      "ints");
  const int padD = safe_downcast<int, int64_t>(padding[0]);
  const int padH =
      padding.size() == 1 ? padD : safe_downcast<int, int64_t>(padding[1]);
  const int padW =
      padding.size() == 1 ? padD : safe_downcast<int, int64_t>(padding[2]);

  TORCH_CHECK(
      dilation.size() == 1 || dilation.size() == 3,
      "max_pool3d: dilation must be either a single int, or a tuple of three "
      "ints");
  const int dilationD = safe_downcast<int, int64_t>(dilation[0]);
  const int dilationH = dilation.size() == 1
      ? dilationD
      : safe_downcast<int, int64_t>(dilation[1]);
  const int dilationW = dilation.size() == 1
      ? dilationD
      : safe_downcast<int, int64_t>(dilation[2]);

  TORCH_CHECK(
      (input.ndimension() == 4 || input.ndimension() == 5),
      "non-empty 4D or 5D (batch mode) tensor expected for input");

  /* sizes */
  const int64_t nbatch = input.ndimension() == 5 ? input.size(-5) : 1;
  const int64_t nblock = input.size(-4);
  const int64_t inputDepth = input.size(-3);
  const int64_t inputHeight = input.size(-2);
  const int64_t inputWidth = input.size(-1);

  const int64_t outputDepth = pooling_output_shape<int64_t>(
      inputDepth, kD, padD, dD, dilationD, ceil_mode);
  const int64_t outputHeight = pooling_output_shape<int64_t>(
      inputHeight, kH, padH, dH, dilationH, ceil_mode);
  const int64_t outputWidth = pooling_output_shape<int64_t>(
      inputWidth, kW, padW, dW, dilationW, ceil_mode);

  pool3d_shape_check(
      input,
      nblock,
      kD,
      kH,
      kW,
      dD,
      dH,
      dW,
      padD,
      padH,
      padW,
      dilationD,
      dilationH,
      dilationW,
      inputDepth,
      inputHeight,
      inputWidth,
      outputDepth,
      outputHeight,
      outputWidth,
      "max_pool3d_with_indices_out_template()",
      /*check_input_size=*/true);

  auto real_eng =
      choose_compute_eng(torch_ipex::xpu::COMPUTE_ENG::BASIC, input);
  if (torch_ipex::xpu::COMPUTE_ENG::ONEDNN == real_eng ||
      input.is_quantized()) { // oneDNN path
    Tensor input_;
    if (input.ndimension() == 4) {
      // 4D: Input (C, D, H, W),  Output (C, D0, H0, W0)
      // cannot give channels last for 4D tensor from frontend user perspective
      // the 2nd dim is outputDepth, not channel dim
      input_ = input.contiguous();
      output.resize_({nblock, outputDepth, outputHeight, outputWidth});
      indices.resize_({nblock, outputDepth, outputHeight, outputWidth});
    } else {
      // 5D: Input (N, C, D, H, W),  Output (N, C, D0, H0, W0)
      // smf supports ChannelsLast3D and Contiguous cases.
      auto smf = input.suggest_memory_format();
      input_ = contiguous_if_needed(input, smf);
      output.resize_(
          {nbatch, nblock, outputDepth, outputHeight, outputWidth}, smf);
      indices.resize_(
          {nbatch, nblock, outputDepth, outputHeight, outputWidth}, smf);
    }

    std::vector<int64_t> kernel_size_vec = {kD, kH, kW};
    std::vector<int64_t> stride_vec = {dD, dH, dW};
    std::vector<int64_t> padding_vec = {padD, padH, padW};
    // per oneDNN definition, no dilation means dilation ratio is 0.
    // Since dilation is already designed in the output size, no dilation
    // is used in torch_ipex::xpu::oneDNN::pooling
    std::vector<int64_t> dilation_vec = {0, 0, 0};
    torch_ipex::xpu::oneDNN::pooling<alg::pooling_max>(
        output,
        indices,
        input_,
        nbatch,
        nblock,
        inputDepth,
        inputHeight,
        inputWidth,
        outputDepth,
        outputHeight,
        outputWidth,
        stride_vec,
        kernel_size_vec,
        dilation_vec,
        padding_vec,
        padding_vec);

  } else { // SYCL implementation
    bool channels_last = input.ndimension() == 5 &&
        input.suggest_memory_format() == at::MemoryFormat::ChannelsLast3d;
    Tensor _input = input;
    if (input.ndimension() == 4) {
      Tensor input_channels_last_check = input.unsqueeze(0);
      // work around buggy behavior of suggest_memory_format here where
      // suggested format of unsqueezed tensor is contiguous while it is
      // really only contiguous in ChannelsLast3d
      channels_last = (!input_channels_last_check.is_contiguous()) &&
          input_channels_last_check.is_contiguous(
              at::MemoryFormat::ChannelsLast3d);
      if (!channels_last) {
        output.resize_({nblock, outputDepth, outputHeight, outputWidth});
        indices.resize_({nblock, outputDepth, outputHeight, outputWidth});
      } else {
        _input = input_channels_last_check;
        output.resize_(
            {1, nblock, outputDepth, outputHeight, outputWidth},
            at::MemoryFormat::ChannelsLast3d);
        indices.resize_(
            {1, nblock, outputDepth, outputHeight, outputWidth},
            at::MemoryFormat::ChannelsLast3d);
        output = output.squeeze(0);
        indices = indices.squeeze(0);
      }
    } else {
      if (!channels_last) {
        output.resize_(
            {nbatch, nblock, outputDepth, outputHeight, outputWidth});
        indices.resize_(
            {nbatch, nblock, outputDepth, outputHeight, outputWidth});
      } else {
        output.resize_(
            {nbatch, nblock, outputDepth, outputHeight, outputWidth},
            at::MemoryFormat::ChannelsLast3d);
        indices.resize_(
            {nbatch, nblock, outputDepth, outputHeight, outputWidth},
            at::MemoryFormat::ChannelsLast3d);
      }
    }

    Tensor format_input;
    Tensor format_output = output;
    if (!channels_last) {
      format_input = input.contiguous();
    } else {
      format_input = _input.contiguous(at::MemoryFormat::ChannelsLast3d);
    }
    Tensor format_indices = indices;

    IPEX_DISPATCH_FLOATING_TYPES_AND2(
        kHalf,
        kBFloat16,
        input.scalar_type(),
        "max_pool3d_with_indices_out_frame",
        [&] {
          scalar_t* input_data = format_input.data_ptr<scalar_t>();
          if (!channels_last) {
            max_pool3d_with_indices_out_frame_impl<scalar_t, false>(
                input_data,
                format_output.data_ptr<scalar_t>(),
                format_indices.data_ptr<int64_t>(),
                nblock, // features
                inputDepth,
                inputHeight,
                inputWidth,
                nbatch,
                outputDepth,
                outputHeight,
                outputWidth,
                kD,
                kH,
                kW,
                dD,
                dH,
                dW,
                padD,
                padH,
                padW,
                dilationD,
                dilationH,
                dilationW);
          } else {
            max_pool3d_with_indices_out_frame_impl<scalar_t, true>(
                input_data,
                format_output.data_ptr<scalar_t>(),
                format_indices.data_ptr<int64_t>(),
                nblock, // features
                inputDepth,
                inputHeight,
                inputWidth,
                nbatch,
                outputDepth,
                outputHeight,
                outputWidth,
                kD,
                kH,
                kW,
                dD,
                dH,
                dW,
                padD,
                padH,
                padW,
                dilationD,
                dilationH,
                dilationW);
          }
        });
  }
}

void max_pool3d_with_indices_backward_out_template(
    Tensor& gradInput,
    const Tensor& gradOutput,
    const Tensor& input,
    const Tensor& indices,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    IntArrayRef dilation,
    bool ceil_mode) {
  TORCH_CHECK(
      kernel_size.size() == 1 || kernel_size.size() == 3,
      "max_pool3d: kernel_size must either be a single int, or a tuple of "
      "three ints");
  const int kD = safe_downcast<int, int64_t>(kernel_size[0]);
  const int kH = kernel_size.size() == 1
      ? kD
      : safe_downcast<int, int64_t>(kernel_size[1]);
  const int kW = kernel_size.size() == 1
      ? kD
      : safe_downcast<int, int64_t>(kernel_size[2]);
  std::vector<int64_t> kernel_vec = {kD, kH, kW};

  TORCH_CHECK(
      stride.empty() || stride.size() == 1 || stride.size() == 3,
      "max_pool3d: stride must either be omitted, a single int, or a tuple of "
      "three ints");
  const int dD = stride.empty() ? kD : safe_downcast<int, int64_t>(stride[0]);
  const int dH = stride.empty() ? kH
      : stride.size() == 1      ? dD
                                : safe_downcast<int, int64_t>(stride[1]);
  const int dW = stride.empty() ? kW
      : stride.size() == 1      ? dD
                                : safe_downcast<int, int64_t>(stride[2]);

  TORCH_CHECK(
      padding.size() == 1 || padding.size() == 3,
      "max_pool3d: padding must either be a single int, or a tuple of three "
      "ints");
  const int padD = safe_downcast<int, int64_t>(padding[0]);
  const int padH =
      padding.size() == 1 ? padD : safe_downcast<int, int64_t>(padding[1]);
  const int padW =
      padding.size() == 1 ? padD : safe_downcast<int, int64_t>(padding[2]);
  std::vector<int64_t> padding_vec = {padD, padH, padW};

  TORCH_CHECK(
      dilation.size() == 1 || dilation.size() == 3,
      "max_pool3d: dilation must be either a single int, or a tuple of three "
      "ints");
  const int dilationD = safe_downcast<int, int64_t>(dilation[0]);
  const int dilationH = dilation.size() == 1
      ? dilationD
      : safe_downcast<int, int64_t>(dilation[1]);
  const int dilationW = dilation.size() == 1
      ? dilationD
      : safe_downcast<int, int64_t>(dilation[2]);
  std::vector<int64_t> stride_vec = {dD, dH, dW};

  TORCH_CHECK(
      (input.ndimension() == 4 || input.ndimension() == 5),
      "non-empty 4D or 5D (batch mode) tensor expected for input");

  TORCH_CHECK(
      (gradOutput.ndimension() == 4 || gradOutput.ndimension() == 5),
      "non-empty 4D or 5D (batch mode) tensor expected for gradOutput");

  /* sizes */
  const int64_t nbatch = input.ndimension() == 5 ? input.size(-5) : 1;
  const int64_t nblock = input.size(-4);
  const int64_t gradInputDepth = input.size(-3);
  const int64_t gradInputHeight = input.size(-2);
  const int64_t gradInputWidth = input.size(-1);

  const int64_t gradOutputDepth = gradOutput.size(-3);
  const int64_t gradOutputHeight = gradOutput.size(-2);
  const int64_t gradOutputWidth = gradOutput.size(-1);

  max_pool3d_backward_shape_check(
      input,
      gradOutput,
      indices,
      nblock,
      kD,
      kH,
      kW,
      dD,
      dH,
      dW,
      padD,
      padH,
      padW,
      dilationD,
      dilationH,
      dilationW,
      gradInputDepth,
      gradInputHeight,
      gradInputWidth,
      gradOutputDepth,
      gradOutputHeight,
      gradOutputWidth,
      "max_pool3d_with_indices_backward_out_template()");

  auto compute_eng = Settings::I().get_compute_eng();
  if (IPEX_ANY(torch_ipex::xpu::oneDNN::is_onednn_layout, gradOutput, input) ||
      compute_eng == torch_ipex::xpu::COMPUTE_ENG::ONEDNN) { // oneDNN path
    // per oneDNN definition, no dilation means dilation ratio is 0.
    // Since dilation is already designed in the output size, no dilation
    // is used in torch_ipex::xpu::oneDNN::pooling
    std::vector<int64_t> dilation_vec = {0, 0, 0};
    torch_ipex::xpu::oneDNN::pooling_backward<alg::pooling_max>(
        gradInput,
        gradOutput,
        input,
        indices,
        nbatch,
        nblock,
        gradInputDepth,
        gradInputHeight,
        gradInputWidth,
        gradOutputDepth,
        gradOutputHeight,
        gradOutputWidth,
        stride_vec,
        kernel_vec,
        dilation_vec,
        padding_vec,
        padding_vec);
  } else {
    // Resize and initialize result tensor.
    bool channels_last = input.ndimension() == 5 &&
        input.suggest_memory_format() == at::MemoryFormat::ChannelsLast3d;
    Tensor _input = input;
    if (input.ndimension() == 4) {
      Tensor input_channels_last_check = input.unsqueeze(0);
      // work around buggy behavior of suggest_memory_format here where
      // suggested format of unsqueezed tensor is contiguous while it is
      // really only contiguous in ChannelsLast3d
      channels_last = (!input_channels_last_check.is_contiguous()) &&
          input_channels_last_check.is_contiguous(
              at::MemoryFormat::ChannelsLast3d);
      if (channels_last) {
        _input = input_channels_last_check;
      }
    }
    if (!channels_last) {
      gradInput.resize_as_(input);
    } else {
      gradInput.resize_as_(_input, at::MemoryFormat::ChannelsLast3d);
    }

    gradInput.zero_();
    if (gradOutput.numel() == 0) {
      return;
    }

    Tensor format_grad_input = gradInput;
    Tensor format_grad_output;
    Tensor format_indices;
    if (!channels_last) {
      format_grad_output = gradOutput.contiguous();
      format_indices = indices.contiguous();
    } else {
      if (input.ndimension() == 4) {
        format_grad_output = gradOutput.unsqueeze(0).contiguous(
            at::MemoryFormat::ChannelsLast3d);
        format_indices =
            indices.unsqueeze(0).contiguous(at::MemoryFormat::ChannelsLast3d);
      } else {
        format_grad_output =
            gradOutput.contiguous(at::MemoryFormat::ChannelsLast3d);
        format_indices = indices.contiguous(at::MemoryFormat::ChannelsLast3d);
      }
    }
    IPEX_DISPATCH_FLOATING_TYPES_AND2(
        kHalf,
        kBFloat16,
        input.scalar_type(),
        "max_pool3d_with_indices_backward_out_frame",
        [&] {
          scalar_t* grad_input_data = format_grad_input.data_ptr<scalar_t>();
          if (!channels_last) {
            max_pool3d_with_indices_backward_out_frame_impl<scalar_t, false>(
                grad_input_data,
                format_grad_output.data_ptr<scalar_t>(),
                format_indices.data_ptr<int64_t>(),
                nblock,
                gradInputDepth,
                gradInputHeight,
                gradInputWidth,
                nbatch,
                gradOutputDepth,
                gradOutputHeight,
                gradOutputWidth);
          } else {
            max_pool3d_with_indices_backward_out_frame_impl<scalar_t, true>(
                grad_input_data,
                format_grad_output.data_ptr<scalar_t>(),
                format_indices.data_ptr<int64_t>(),
                nblock,
                gradInputDepth,
                gradInputHeight,
                gradInputWidth,
                nbatch,
                gradOutputDepth,
                gradOutputHeight,
                gradOutputWidth);
          }
        });
  }
}

} // namespace impl

std::tuple<Tensor&, Tensor&> max_pool3d_with_indices_out(
    const Tensor& self,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    IntArrayRef dilation,
    bool ceil_mode,
    Tensor& out,
    Tensor& indices) {
  impl::max_pool3d_with_indices_out_template(
      out, indices, self, kernel_size, stride, padding, dilation, ceil_mode);
  return std::tuple<Tensor&, Tensor&>(out, indices);
}

std::tuple<Tensor, Tensor> max_pool3d_with_indices(
    const Tensor& self,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    IntArrayRef dilation,
    bool ceil_mode) {
  Tensor output = at::empty({0}, self.options());
  Tensor indices = at::empty({0}, self.options().dtype(kLong));
  return at::AtenIpexTypeXPU::max_pool3d_with_indices_out(
      self, kernel_size, stride, padding, dilation, ceil_mode, output, indices);
}

Tensor& max_pool3d_with_indices_backward_out(
    const Tensor& grad_output_,
    const Tensor& self_,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    IntArrayRef dilation,
    bool ceil_mode,
    const Tensor& indices_,
    Tensor& grad_input) {
  Tensor self, grad_output, indices;
  if (self_.ndimension() == 4) {
    // 4D: Input (C, D, H, W),  Output (C, D0, H0, W0)
    // cannot give channels last for 4D tensor from frontend user perspective
    // the 2nd dim is outputDepth, not channel dim
    self = self_.contiguous();
    grad_output = grad_output_.contiguous();
    indices = indices_.contiguous();
    grad_input.resize_as_(self);
  } else {
    // 5D: Input (N, C, D, H, W),  Output (N, C, D0, H0, W0)
    // smf supports ChannelsLast3D and Contiguous cases.
    auto smf = self_.suggest_memory_format();
    self = contiguous_if_needed(self_, smf);
    grad_output = contiguous_if_needed(grad_output_, smf);
    indices = contiguous_if_needed(indices_, smf);
    grad_input.resize_as_(self, smf);
  }
  impl::max_pool3d_with_indices_backward_out_template(
      grad_input,
      grad_output,
      self,
      indices,
      kernel_size,
      stride,
      padding,
      dilation,
      ceil_mode);
  return grad_input;
}

Tensor max_pool3d_with_indices_backward(
    const Tensor& grad_output_,
    const Tensor& self_,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    IntArrayRef dilation,
    bool ceil_mode,
    const Tensor& indices_) {
  Tensor self, grad_output, indices, grad_input;
  if (self_.ndimension() == 4) {
    // 4D: Input (C, D, H, W),  Output (C, D0, H0, W0)
    // cannot give channels last for 4D tensor from frontend user perspective
    // the 2nd dim is outputDepth, not channel dim
    self = self_.contiguous();
    grad_output = grad_output_.contiguous();
    indices = indices_.contiguous();
    grad_input = at::empty_like(self);
  } else {
    // 5D: Input (N, C, D, H, W),  Output (N, C, D0, H0, W0)
    // smf supports ChannelsLast3D and Contiguous cases.
    auto smf = self_.suggest_memory_format();
    self = contiguous_if_needed(self_, smf);
    grad_output = contiguous_if_needed(grad_output_, smf);
    indices = contiguous_if_needed(indices_, smf);
    grad_input = at::empty_like(self, smf);
  }
  impl::max_pool3d_with_indices_backward_out_template(
      grad_input,
      grad_output,
      self,
      indices,
      kernel_size,
      stride,
      padding,
      dilation,
      ceil_mode);
  return grad_input;
}

} // namespace AtenIpexTypeXPU
} // namespace at
