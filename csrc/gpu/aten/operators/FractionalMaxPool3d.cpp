#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/TensorUtils.h>
#include <ATen/Utils.h>
#include <core/Memory.h>
#include <core/MemoryFormat.h>
#include <runtime/Utils.h>
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/Atomics.h"
#include "comm/RegistrationDeclarations.h"
#ifdef USE_OVERRIDE_OP
#include <ATen/DeviceGuard.h>
#include <ATen/core/op_registration/adaption.h>
#include "comm/RegisterUtils.h"
#include "utils/CustomOperatorRegistration.h"
#endif

using namespace torch_ipex::xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

template <typename scalar_t, typename accscalar_t>
inline int64_t get_intervals(
    accscalar_t sample,
    int64_t index,
    int64_t inputSize,
    int64_t outputSize,
    int64_t poolSize) {
  accscalar_t alpha = static_cast<accscalar_t>(inputSize - poolSize) /
      static_cast<accscalar_t>(outputSize - 1);
  if (index == outputSize - 1) {
    return inputSize - poolSize;
  } else {
    return static_cast<int64_t>((index + sample) * alpha) -
        static_cast<int64_t>(sample * alpha);
  }
}

template <typename scalar_t, typename accscalar_t>
struct FractionalMaxPool3dOutFrameCfFunctor {
  void operator()(sycl::nd_item<3> item) const {
    auto input_ptr = input_data;
    auto output_ptr = output_data;
    auto indices_ptr = indices_data;
    auto samples_ptr = samples_data;

    int ourOutputPoint = item.get_global_id()[2];
    int batch = item.get_group()[0];
    int plane = item.get_group()[1];

    if (ourOutputPoint < outputPlaneSize) {
      int64_t outputT = ourOutputPoint / (outputSizeH * outputSizeW);
      int64_t outputH = (ourOutputPoint / outputSizeW) % outputSizeH;
      int64_t outputW = ourOutputPoint % outputSizeW;

      int64_t poolT = get_intervals<scalar_t, accscalar_t>(
          static_cast<accscalar_t>(
              samples_ptr
                  [batch * numPlane * 3 + plane * 3] /*[batch][plane][0]*/),
          outputT,
          inputSizeT,
          outputSizeT,
          poolSizeT);
      int64_t poolH = get_intervals<scalar_t, accscalar_t>(
          static_cast<accscalar_t>(
              samples_ptr
                  [batch * numPlane * 3 + plane * 3 + 1] /*[batch][plane][1]*/),
          outputH,
          inputSizeH,
          outputSizeH,
          poolSizeH);
      int64_t poolW = get_intervals<scalar_t, accscalar_t>(
          static_cast<accscalar_t>(
              samples_ptr
                  [batch * numPlane * 3 + plane * 3 + 2] /*[batch][plane][2]*/),
          outputW,
          inputSizeW,
          outputSizeW,
          poolSizeW);

      scalar_t maxVal = std::numeric_limits<scalar_t>::lowest();
      int64_t maxIndex = -1;

      for (int64_t t = poolT; t < poolT + poolSizeT; ++t) {
        for (int64_t h = poolH; h < poolH + poolSizeH; ++h) {
          for (int64_t w = poolW; w < poolW + poolSizeW; ++w) {
            int64_t load_offset = batch * ibatch_stride +
                plane * iplane_stride + t * iT_stride + h * inputSizeW + w;
            scalar_t val = input_ptr[load_offset] /*[batch][plane][t][h][w]*/;
            if (val > maxVal) {
              maxIndex = t * inputSizeH * inputSizeW + h * inputSizeW + w;
              maxVal = val;
            }
          }
        }
      }

      int64_t store_offset = batch * obatch_stride + plane * oplane_stride +
          outputT * oT_stride + outputH * outputSizeW + outputW;
      indices_ptr[store_offset] /*[batch][plane][outputT][outputH][outputW]*/
          = maxIndex;
      output_ptr[store_offset] /*[batch][plane][outputT][outputH][outputW]*/
          = maxVal;
    }
  }
  FractionalMaxPool3dOutFrameCfFunctor(
      scalar_t* output_data_,
      int64_t* indices_data_,
      scalar_t* input_data_,
      scalar_t* samples_data_,
      int numBatch_,
      int numPlane_,
      int inputSizeT_,
      int inputSizeH_,
      int inputSizeW_,
      int outputSizeT_,
      int outputSizeH_,
      int outputSizeW_,
      int poolSizeT_,
      int poolSizeH_,
      int poolSizeW_,
      int outputPlaneSize_,
      int64_t iT_stride_,
      int64_t iplane_stride_,
      int64_t ibatch_stride_,
      int64_t oT_stride_,
      int64_t oplane_stride_,
      int64_t obatch_stride_)
      : output_data(output_data_),
        indices_data(indices_data_),
        input_data(input_data_),
        samples_data(samples_data_),
        numBatch(numBatch_),
        numPlane(numPlane_),
        inputSizeT(inputSizeT_),
        inputSizeH(inputSizeH_),
        inputSizeW(inputSizeW_),
        outputSizeT(outputSizeT_),
        outputSizeH(outputSizeH_),
        outputSizeW(outputSizeW_),
        poolSizeT(poolSizeT_),
        poolSizeH(poolSizeH_),
        poolSizeW(poolSizeW_),
        outputPlaneSize(outputPlaneSize_),
        iT_stride(iT_stride_),
        iplane_stride(iplane_stride_),
        ibatch_stride(ibatch_stride_),
        oT_stride(oT_stride_),
        oplane_stride(oplane_stride_),
        obatch_stride(obatch_stride_) {}

 private:
  scalar_t* output_data;
  int64_t* indices_data;
  scalar_t* input_data;
  scalar_t* samples_data;
  int numBatch;
  int numPlane;
  int inputSizeT;
  int inputSizeH;
  int inputSizeW;
  int outputSizeT;
  int outputSizeH;
  int outputSizeW;
  int poolSizeT;
  int poolSizeH;
  int poolSizeW;
  int outputPlaneSize;
  int64_t iT_stride;
  int64_t iplane_stride;
  int64_t ibatch_stride;
  int64_t oT_stride;
  int64_t oplane_stride;
  int64_t obatch_stride;
};

template <typename scalar_t>
void fractional_max_pool3d_out_frame_cf(
    scalar_t* output,
    int64_t* indices,
    scalar_t* input,
    scalar_t* samples,
    int numBatch,
    int numPlane,
    int inputSizeT,
    int inputSizeH,
    int inputSizeW,
    int outputSizeT,
    int outputSizeH,
    int outputSizeW,
    int poolSizeT,
    int poolSizeH,
    int poolSizeW) {
  using accscalar_t = acc_type<scalar_t>;
  auto& queue = dpcppGetCurrentQueue();
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  int64_t max_wg_size = dpcppMaxWorkGroupSize(dev_id);
  int outputPlaneSize = outputSizeH * outputSizeW * outputSizeT;
  // input stride for NCTHW data
  int64_t iT_stride = inputSizeH * inputSizeW;
  int64_t iplane_stride = inputSizeT * iT_stride;
  int64_t ibatch_stride = numPlane * iplane_stride;
  // output stride for NCTHW data
  int64_t oT_stride = outputSizeH * outputSizeW;
  int64_t oplane_stride = outputSizeT * oT_stride;
  int64_t obatch_stride = numPlane * oplane_stride;

  int work_group_size =
      outputPlaneSize > max_wg_size ? max_wg_size : outputPlaneSize;
  int work_group_num =
      (outputPlaneSize + work_group_size - 1) / work_group_size;

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto input_data = input;
    auto output_data = output;
    auto indices_data = indices;
    auto samples_data = samples;
    FractionalMaxPool3dOutFrameCfFunctor<scalar_t, accscalar_t> kfn(
        output_data,
        indices_data,
        input_data,
        samples_data,
        numBatch,
        numPlane,
        inputSizeT,
        inputSizeH,
        inputSizeW,
        outputSizeT,
        outputSizeH,
        outputSizeW,
        poolSizeT,
        poolSizeH,
        poolSizeW,
        outputPlaneSize,
        iT_stride,
        iplane_stride,
        ibatch_stride,
        oT_stride,
        oplane_stride,
        obatch_stride);
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<3>(
            sycl::range<3>(
                numBatch, numPlane, work_group_size * work_group_num),
            sycl::range<3>(1, 1, work_group_size)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

template <typename scalar_t, typename accscalar_t>
struct FractionalMaxPool3dOutFrameClFunctor {
  void operator()(sycl::nd_item<3> item) const {
    auto input_ptr = input_data;
    auto output_ptr = output_data;
    auto indices_ptr = indices_data;
    auto samples_ptr = samples_data;

    int outputIndex = item.get_global_id()[2];

    if (outputIndex < outputSize) {
      int batch = item.get_group()[0];
      int outputT = item.get_group()[1];
      int outputH = outputIndex / numPlane / outputSizeW % outputSizeH;
      int outputW = outputIndex / numPlane % outputSizeW;
      int plane = outputIndex % numPlane;
      int64_t poolT = get_intervals<scalar_t, accscalar_t>(
          static_cast<accscalar_t>(
              samples_ptr
                  [batch * numPlane * 3 + plane * 3] /*[batch][plane][0]*/),
          outputT,
          inputSizeT,
          outputSizeT,
          poolSizeT);
      int64_t poolH = get_intervals<scalar_t, accscalar_t>(
          static_cast<accscalar_t>(
              samples_ptr
                  [batch * numPlane * 3 + plane * 3 + 1] /*[batch][plane][1]*/),
          outputH,
          inputSizeH,
          outputSizeH,
          poolSizeH);
      int64_t poolW = get_intervals<scalar_t, accscalar_t>(
          static_cast<accscalar_t>(
              samples_ptr
                  [batch * numPlane * 3 + plane * 3 + 2] /*[batch][plane][2]*/),
          outputW,
          inputSizeW,
          outputSizeW,
          poolSizeW);

      scalar_t maxVal = std::numeric_limits<scalar_t>::lowest();
      int64_t maxIndex = -1;

      for (int64_t t = poolT; t < poolT + poolSizeT; ++t) {
        for (int64_t h = poolH; h < poolH + poolSizeH; ++h) {
          for (int64_t w = poolW; w < poolW + poolSizeW; ++w) {
            int64_t load_offset = batch * iBatch_stride + t * iT_stride +
                h * iH_stride + w * numPlane + plane;
            scalar_t val = input_ptr[load_offset] /*[batch][plane][t][h][w]*/;
            if (val > maxVal) {
              maxIndex = t * inputSizeH * inputSizeW + h * inputSizeW + w;
              maxVal = val;
            }
          }
        }
      }
      int64_t store_offset = batch * oBatch_stride + outputT * oT_stride +
          outputH * oH_stride + outputW * numPlane + plane;
      indices_ptr[store_offset] /*[batch][plane][outputT][outputH][outputW]*/
          = maxIndex;
      output_ptr[store_offset] /*[batch][plane][outputT][outputH][outputW]*/
          = maxVal;
    }
  }
  FractionalMaxPool3dOutFrameClFunctor(
      scalar_t* output_data_,
      int64_t* indices_data_,
      scalar_t* input_data_,
      scalar_t* samples_data_,
      int numBatch_,
      int numPlane_,
      int inputSizeT_,
      int inputSizeH_,
      int inputSizeW_,
      int outputSizeT_,
      int outputSizeH_,
      int outputSizeW_,
      int poolSizeT_,
      int poolSizeH_,
      int poolSizeW_,
      int outputSize_,
      int64_t iH_stride_,
      int64_t iT_stride_,
      int64_t iBatch_stride_,
      int64_t oH_stride_,
      int64_t oT_stride_,
      int64_t oBatch_stride_)
      : output_data(output_data_),
        indices_data(indices_data_),
        input_data(input_data_),
        samples_data(samples_data_),
        numBatch(numBatch_),
        numPlane(numPlane_),
        inputSizeT(inputSizeT_),
        inputSizeH(inputSizeH_),
        inputSizeW(inputSizeW_),
        outputSizeT(outputSizeT_),
        outputSizeH(outputSizeH_),
        outputSizeW(outputSizeW_),
        poolSizeT(poolSizeT_),
        poolSizeH(poolSizeH_),
        poolSizeW(poolSizeW_),
        outputSize(outputSize_),
        iH_stride(iH_stride_),
        iT_stride(iT_stride_),
        iBatch_stride(iBatch_stride_),
        oH_stride(oH_stride_),
        oT_stride(oT_stride_),
        oBatch_stride(oBatch_stride_) {}

 private:
  scalar_t* output_data;
  int64_t* indices_data;
  scalar_t* input_data;
  scalar_t* samples_data;
  int numBatch;
  int numPlane;
  int inputSizeT;
  int inputSizeH;
  int inputSizeW;
  int outputSizeT;
  int outputSizeH;
  int outputSizeW;
  int poolSizeT;
  int poolSizeH;
  int poolSizeW;
  int outputSize;
  int64_t iH_stride;
  int64_t iT_stride;
  int64_t iBatch_stride;
  int64_t oH_stride;
  int64_t oT_stride;
  int64_t oBatch_stride;
};

template <typename scalar_t>
void fractional_max_pool3d_out_frame_cl(
    scalar_t* output,
    int64_t* indices,
    scalar_t* input,
    scalar_t* samples,
    int numBatch,
    int numPlane,
    int inputSizeT,
    int inputSizeH,
    int inputSizeW,
    int outputSizeT,
    int outputSizeH,
    int outputSizeW,
    int poolSizeT,
    int poolSizeH,
    int poolSizeW) {
  using accscalar_t = acc_type<scalar_t>;
  auto& queue = dpcppGetCurrentQueue();
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  int64_t max_wg_size = dpcppMaxWorkGroupSize(dev_id);
  int outputSize = outputSizeH * outputSizeW * numPlane;
  // input stride for NTHWC data
  int64_t iH_stride = inputSizeW * numPlane;
  int64_t iT_stride = inputSizeH * iH_stride;
  // iBatch_stride = inputSizeT * inputSizeH * inputSizeW * numPlane
  int64_t iBatch_stride = inputSizeT * iT_stride;

  // output stride for NTHWC data
  int64_t oH_stride = outputSizeW * numPlane;
  int64_t oT_stride = outputSizeH * oH_stride;
  // oBatch_stride = outputSizeT * outputSizeH * outputSizeW * numPlane
  int64_t oBatch_stride = outputSizeT * oT_stride;

  int work_group_size = outputSize > max_wg_size ? max_wg_size : outputSize;
  int work_group_num = (outputSize + work_group_size - 1) / work_group_size;

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto input_data = input;
    auto output_data = output;
    auto indices_data = indices;
    auto samples_data = samples;
    FractionalMaxPool3dOutFrameClFunctor<scalar_t, accscalar_t> kfn(
        output_data,
        indices_data,
        input_data,
        samples_data,
        numBatch,
        numPlane,
        inputSizeT,
        inputSizeH,
        inputSizeW,
        outputSizeT,
        outputSizeH,
        outputSizeW,
        poolSizeT,
        poolSizeH,
        poolSizeW,
        outputSize,
        iH_stride,
        iT_stride,
        iBatch_stride,
        oH_stride,
        oT_stride,
        oBatch_stride);
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<3>(
            sycl::range<3>(
                numBatch, outputSizeT, work_group_size * work_group_num),
            sycl::range<3>(1, 1, work_group_size)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

template <typename scalar_t>
struct FractionalMaxPool3dBackwardOutFrameKernelFunctor {
  void operator()(sycl::nd_item<3> item) const {
    int ourOutputPoint = item.get_global_id()[2];
    int plane = item.get_group()[1];
    int batch = item.get_group()[0];

    if (ourOutputPoint < gradOutputPlaneSize) {
      int64_t outputT = ourOutputPoint / gradOutputSizeH / gradOutputSizeW;
      int64_t outputH = ourOutputPoint / gradOutputSizeW % gradOutputSizeH;
      int64_t outputW = ourOutputPoint % gradOutputSizeW;

      int64_t index = indices[batch][plane][outputT][outputH][outputW];
      assert(index >= 0);
      int64_t inputW = index % gradInputSizeW;
      int64_t inputH = (index / gradInputSizeW % gradInputSizeH);
      int64_t inputT = index / (gradInputSizeH * gradInputSizeW);
      assert(inputT < gradInput.size(2));
      atomicAdd(
          (dpcpp_global_ptr_pt<scalar_t>)&gradInput[batch][plane][inputT]
                                                   [inputH][inputW],
          gradOutput[batch][plane][outputT][outputH][outputW]);
    }
  }
  FractionalMaxPool3dBackwardOutFrameKernelFunctor(
      PackedTensorAccessor64<scalar_t, 5> gradInput_,
      PackedTensorAccessor64<scalar_t, 5> gradOutput_,
      PackedTensorAccessor64<int64_t, 5> indices_,
      int gradOutputSizeH_,
      int gradOutputSizeW_,
      int gradInputSizeH_,
      int gradInputSizeW_,
      int gradOutputPlaneSize_)
      : gradInput(gradInput_),
        gradOutput(gradOutput_),
        indices(indices_),
        gradOutputSizeH(gradOutputSizeH_),
        gradOutputSizeW(gradOutputSizeW_),
        gradInputSizeH(gradInputSizeH_),
        gradInputSizeW(gradInputSizeW_),
        gradOutputPlaneSize(gradOutputPlaneSize_) {}

 private:
  PackedTensorAccessor64<scalar_t, 5> gradInput;
  PackedTensorAccessor64<scalar_t, 5> gradOutput;
  PackedTensorAccessor64<int64_t, 5> indices;
  int gradOutputSizeH;
  int gradOutputSizeW;
  int gradInputSizeH;
  int gradInputSizeW;
  int gradOutputPlaneSize;
};

template <typename scalar_t>
void fractional_max_pool3d_backward_out_frame(
    PackedTensorAccessor64<scalar_t, 5> gradInput,
    PackedTensorAccessor64<scalar_t, 5> gradOutput,
    PackedTensorAccessor64<int64_t, 5> indices) {
  auto numBatch = gradInput.size(0);
  auto numPlane = gradInput.size(1);
  auto gradOutputSizeT = gradOutput.size(2);
  auto gradOutputSizeH = gradOutput.size(3);
  auto gradOutputSizeW = gradOutput.size(4);
  auto gradInputSizeT = gradInput.size(2);
  auto gradInputSizeH = gradInput.size(3);
  auto gradInputSizeW = gradInput.size(4);
  auto& queue = dpcppGetCurrentQueue();
  int gradOutputPlaneSize = gradOutputSizeT * gradOutputSizeH * gradOutputSizeW;
  int work_group_size = gradOutputPlaneSize > 256 ? 256 : gradOutputPlaneSize;
  int work_group_num =
      (gradOutputPlaneSize + work_group_size - 1) / work_group_size;

  auto cgf = DPCPP_Q_CGF(cgh) {
    FractionalMaxPool3dBackwardOutFrameKernelFunctor<scalar_t> kfn(
        gradInput,
        gradOutput,
        indices,
        gradOutputSizeH,
        gradOutputSizeW,
        gradInputSizeH,
        gradInputSizeW,
        gradOutputPlaneSize);
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<3>(
            sycl::range<3>(
                numBatch, numPlane, work_group_size * work_group_num),
            sycl::range<3>(1, 1, work_group_size)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

void fractional_max_pool3d_out_template(
    Tensor& output,
    Tensor& indices,
    const Tensor& input,
    IntArrayRef pool_size,
    IntArrayRef output_size,
    const Tensor& randomSamples) {
  int64_t planeDim = 0;
  int64_t dimt = 1;
  int64_t dimh = 2;
  int64_t dimw = 3;
  int64_t numBatch = 1;

  int64_t outputT = output_size[0];
  int64_t outputH = output_size[1];
  int64_t outputW = output_size[2];
  int64_t poolSizeT = pool_size[0];
  int64_t poolSizeH = pool_size[1];
  int64_t poolSizeW = pool_size[2];

  int64_t ndims = input.ndimension();
  TORCH_CHECK(
      input.numel() != 0 && (ndims == 4 || ndims == 5),
      "fractional_max_pool3d_out_template(): ",
      "non-empty 4D or 5D (batch mode) tensor expected for input, but got: ",
      ndims);

  if (ndims == 5) {
    numBatch = input.size(0);
    planeDim++;
    dimt++;
    dimh++;
    dimw++;
  }

  /* sizes */
  int64_t numPlanes = input.size(planeDim);
  int64_t inputT = input.size(dimt);
  int64_t inputH = input.size(dimh);
  int64_t inputW = input.size(dimw);

  TORCH_CHECK(
      outputT + poolSizeT - 1 < inputT,
      "fractional_max_pool3d_out_template(): ",
      "pool time (",
      poolSizeT,
      ") too large relative to input time (",
      inputT,
      ")");
  TORCH_CHECK(
      outputH + poolSizeH - 1 < inputH,
      "fractional_max_pool3d_out_template(): ",
      "pool height (",
      poolSizeH,
      ") too large relative to input height (",
      inputH,
      ")");
  TORCH_CHECK(
      outputW + poolSizeW - 1 < inputW,
      "fractional_max_pool3d_out_template(): ",
      "pool width (",
      poolSizeW,
      ") too large relative to input width (",
      inputW,
      ")");

  auto smf = (4 == ndims) ? at::MemoryFormat::Contiguous
                          : input.suggest_memory_format();
  if (ndims == 4) {
    /* resize output */
    output.resize_({numPlanes, outputT, outputH, outputW});
    /* indices will contain the locations for each output point */
    indices.resize_({numPlanes, outputT, outputH, outputW});
  } else {
    /* resize output */
    output.resize_({numBatch, numPlanes, outputT, outputH, outputW}, smf);
    /* indices will contain the locations for each output point */
    indices.resize_({numBatch, numPlanes, outputT, outputH, outputW}, smf);
  }

  auto output_ = output;
  auto indices_ = indices;
  auto input_ = input.contiguous(smf);
  if (ndims == 4) {
    output_ = output_.reshape({1, numPlanes, outputT, outputH, outputW});
    indices_ = indices_.reshape({1, numPlanes, outputT, outputH, outputW});
    input_ = input_.reshape({1, numPlanes, inputT, inputH, inputW});
  }
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      input.scalar_type(),
      "fractional_max_pool3d_out_frame",
      [&] {
        if (is_smf_channels_last(input))
          fractional_max_pool3d_out_frame_cl<scalar_t>(
              output_.data_ptr<scalar_t>(),
              indices_.data_ptr<int64_t>(),
              input_.data_ptr<scalar_t>(),
              randomSamples.data_ptr<scalar_t>(),
              input_.size(0),
              input_.size(1),
              inputT,
              inputH,
              inputW,
              outputT,
              outputH,
              outputW,
              poolSizeT,
              poolSizeH,
              poolSizeW);
        else
          fractional_max_pool3d_out_frame_cf<scalar_t>(
              output_.data_ptr<scalar_t>(),
              indices_.data_ptr<int64_t>(),
              input_.data_ptr<scalar_t>(),
              randomSamples.data_ptr<scalar_t>(),
              input_.size(0),
              input_.size(1),
              inputT,
              inputH,
              inputW,
              outputT,
              outputH,
              outputW,
              poolSizeT,
              poolSizeH,
              poolSizeW);
      });
}

void fractional_max_pool3d_backward_out_template(
    Tensor& gradInput,
    const Tensor& gradOutput,
    const Tensor& input,
    IntArrayRef pool_size /* unused */,
    IntArrayRef output_size,
    const Tensor& indices) {
  int64_t dimt = 1;
  int64_t dimh = 2;
  int64_t dimw = 3;

  int64_t outputT = output_size[0];
  int64_t outputH = output_size[1];
  int64_t outputW = output_size[2];

  int64_t ndims = input.ndimension();
  if (ndims == 5) {
    dimt++;
    dimh++;
    dimw++;
  }

  /* sizes */
  int64_t inputT = input.size(dimt);
  int64_t inputH = input.size(dimh);
  int64_t inputW = input.size(dimw);

  TORCH_CHECK(
      outputT == gradOutput.size(dimt),
      "fractional_max_pool3d_backward_out_template(): ",
      "gradOutput time unexpected");
  TORCH_CHECK(
      outputH == gradOutput.size(dimh),
      "fractional_max_pool3d_backward_out_template(): ",
      "gradOutput height unexpected");
  TORCH_CHECK(
      outputW == gradOutput.size(dimw),
      "fractional_max_pool3d_backward_out_template(): ",
      "gradOutput width unexpected");

  /* resize */
  gradInput.resize_as_(input);
  gradInput.zero_();

  auto gradInput_ = gradInput;
  auto gradOutput_ = gradOutput;
  auto indices_ = indices;

  if (ndims == 4) {
    gradInput_ =
        gradInput_.reshape({1, gradInput.size(0), inputT, inputH, inputW});
    gradOutput_ =
        gradOutput_.reshape({1, gradOutput.size(0), outputT, outputH, outputW});
    indices_ =
        indices_.reshape({1, indices.size(0), outputT, outputH, outputW});
  }

  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      gradOutput.scalar_type(),
      "fractional_max_pool3d_backward_out_frame",
      [&] {
        fractional_max_pool3d_backward_out_frame<scalar_t>(
            gradInput_.packed_accessor64<scalar_t, 5>(),
            gradOutput_.packed_accessor64<scalar_t, 5>(),
            indices_.packed_accessor64<int64_t, 5>());
      });
}

} // namespace impl

std::tuple<Tensor&, Tensor&> fractional_max_pool3d_out(
    const Tensor& self,
    IntArrayRef kernel_size,
    IntArrayRef output_size,
    const Tensor& random_samples,
    Tensor& output,
    Tensor& indices) {
  impl::fractional_max_pool3d_out_template(
      output, indices, self, kernel_size, output_size, random_samples);
  return std::tuple<Tensor&, Tensor&>(output, indices);
}

Tensor& fractional_max_pool3d_backward_out(
    const Tensor& grad_output,
    const Tensor& self,
    IntArrayRef kernel_size,
    IntArrayRef output_size,
    const Tensor& indices,
    Tensor& grad_input) {
  impl::fractional_max_pool3d_backward_out_template(
      grad_input, grad_output, self, kernel_size, output_size, indices);
  return grad_input;
}

#ifdef USE_OVERRIDE_OP
void fractional_max_pool3d_meta(
    const at::Tensor& input_,
    IntArrayRef pool_size,
    IntArrayRef output_size,
    const at::Tensor& randomSamples,
    Tensor& output,
    Tensor& indices) {
  TORCH_CHECK(
      pool_size.size() == 3,
      "fractional_max_pool3d: kernel_size must either be a single Int or tuple of three Ints")
  TORCH_CHECK(
      output_size.size() == 3,
      "fractional_max_pool3d: output_size must either be a single Int or tuple of three Ints")
  int64_t outputT = output_size[0];
  int64_t outputH = output_size[1];
  int64_t outputW = output_size[2];
  int64_t poolSizeT = pool_size[0];
  int64_t poolSizeH = pool_size[1];
  int64_t poolSizeW = pool_size[2];

  int64_t numBatch = 1;
  int64_t planeDim = 0;
  int64_t timeDim = 1;
  int64_t heightDim = 2;
  int64_t widthDim = 3;

  int64_t ndims = input_.ndimension();
  TORCH_CHECK(
      ndims == 4 || ndims == 5,
      "fractional_max_pool3d_out(): Expected 4D or 5D tensor, but got: ",
      input_.sizes());
  for (const auto i : c10::irange(1, ndims)) {
    TORCH_CHECK(
        input_.size(i) > 0,
        "fractional_max_pool3d_out(): Expected input to have non-zero size for non-batch dimensions, but got",
        input_.sizes(),
        " with dimension ",
        i,
        " being empty.");
  }

  if (ndims == 5) {
    numBatch = input_.size(0);
    planeDim++;
    timeDim++;
    heightDim++;
    widthDim++;
  }

  /* sizes */
  int64_t numPlanes = input_.size(planeDim);
  int64_t inputT = input_.size(timeDim);
  int64_t inputH = input_.size(heightDim);
  int64_t inputW = input_.size(widthDim);

  TORCH_CHECK(
      outputT + poolSizeT - 1 < inputT,
      "fractional_max_pool3d_out(): pool time ",
      poolSizeT,
      " too large relative to input time ",
      inputT);
  TORCH_CHECK(
      outputW + poolSizeW - 1 < inputW,
      "fractional_max_pool3d_out(): pool width ",
      poolSizeW,
      " too large relative to input width ",
      inputW);
  TORCH_CHECK(
      outputH + poolSizeH - 1 < inputH,
      "fractional_max_pool3d_out(): pool height ",
      poolSizeH,
      " too large relative to input height ",
      inputH);

  if (ndims == 4) {
    if (output.defined()) {
      at::AtenIpexTypeXPU::resize_out(
          output, {numPlanes, outputT, outputH, outputW}, {}, input_.options());
    } else {
      output = at::AtenIpexTypeXPU::create_out(
          {numPlanes, outputT, outputH, outputW}, {}, input_.options());
    }
    /* indices will contain the locations for each output point */
    if (indices.defined()) {
      at::AtenIpexTypeXPU::resize_out(
          indices,
          {numPlanes, outputT, outputH, outputW},
          {},
          input_.options().dtype(kLong));
    } else {
      indices = at::AtenIpexTypeXPU::create_out(
          {numPlanes, outputT, outputH, outputW},
          {},
          input_.options().dtype(kLong));
    }
  } else {
    if (output.defined()) {
      at::AtenIpexTypeXPU::resize_out(
          output,
          {numBatch, numPlanes, outputT, outputH, outputW},
          {},
          input_.options());
    } else {
      output = at::AtenIpexTypeXPU::create_out(
          {numBatch, numPlanes, outputT, outputH, outputW},
          {},
          input_.options());
    }
    /* indices will contain the locations for each output point */
    if (indices.defined()) {
      at::AtenIpexTypeXPU::resize_out(
          indices,
          {numBatch, numPlanes, outputT, outputH, outputW},
          {},
          input_.options().dtype(kLong));
    } else {
      indices = at::AtenIpexTypeXPU::create_out(
          {numBatch, numPlanes, outputT, outputH, outputW},
          {},
          input_.options().dtype(kLong));
    }
  }
}
std::tuple<Tensor, Tensor> fractional_max_pool3d(
    const Tensor& self,
    IntArrayRef kernel_size,
    IntArrayRef output_size,
    const Tensor& random_samples) {
  Tensor output;
  Tensor indices;
  fractional_max_pool3d_meta(
      self, kernel_size, output_size, random_samples, output, indices);
  impl::fractional_max_pool3d_out_template(
      output, indices, self, kernel_size, output_size, random_samples);
  return std::tuple<Tensor&, Tensor&>(output, indices);
}
#endif

Tensor fractional_max_pool3d_backward(
    const Tensor& grad_output,
    const Tensor& self,
    IntArrayRef kernel_size,
    IntArrayRef output_size,
    const Tensor& indices) {
  Tensor grad_input = at::empty({0}, self.options());
  impl::fractional_max_pool3d_backward_out_template(
      grad_input, grad_output, self, kernel_size, output_size, indices);
  return grad_input;
}

} // namespace AtenIpexTypeXPU
} // namespace at

#ifdef USE_OVERRIDE_OP
namespace {
::std::tuple<at::Tensor, at::Tensor> wrapper_XPU_fractional_max_pool3d(
    const at::Tensor& self,
    at::IntArrayRef kernel_size,
    at::IntArrayRef output_size,
    const at::Tensor& random_samples) {
  std::optional<Device> common_device = std::nullopt;
  (void)common_device; // Suppress unused variable warning
  c10::impl::check_and_update_common_device(
      common_device, self, "wrapper_XPU_fractional_max_pool3d", "self");
  c10::impl::check_and_update_common_device(
      common_device,
      random_samples,
      "wrapper_XPU_fractional_max_pool3d",
      "random_samples");
  const OptionalDeviceGuard device_guard(device_of(self));

  return at::AtenIpexTypeXPU::fractional_max_pool3d(
      self, kernel_size, output_size, random_samples);
}
IPEX_TORCH_LIBRARY_IMPL(aten, XPU, m) {
  m.impl(
      "fractional_max_pool3d", TORCH_FN((&wrapper_XPU_fractional_max_pool3d)));
}

} // namespace
#endif
