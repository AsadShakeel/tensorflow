/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// See docs in ../ops/nn_ops.cc.

#ifdef INTEL_MKL

#include <algorithm>
#include <vector>

#include "tensorflow/core/framework/numeric_op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_slice.h"
#include "tensorflow/core/kernels/conv_grad_ops.h"
#include "tensorflow/core/kernels/mkl_conv_ops.h"
#include "tensorflow/core/kernels/ops_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/util/padding.h"
#include "tensorflow/core/util/tensor_format.h"
#include "tensorflow/core/util/use_cudnn.h"
#include "tensorflow/core/util/work_sharder.h"

#include "tensorflow/core/util/mkl_util.h"
#include "mkl_dnn.h"
#include "mkl_dnn_types.h"

#ifdef INTEL_MKL_DNN
#include "mkldnn.hpp"

using mkldnn::stream;
using mkldnn::prop_kind;

using mkldnn::convolution_forward;
using mkldnn::convolution_backward_weights;
using mkldnn::convolution_direct;

#endif

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;

#ifndef INTEL_MKL_DNN

template <typename Device, class T>
class MklConv2DCustomBackpropFilterOp : public OpKernel {
 public:
  explicit MklConv2DCustomBackpropFilterOp(OpKernelConstruction* context)
      : OpKernel(context) {
    string data_format;
    OP_REQUIRES_OK(context, context->GetAttr("data_format", &data_format));
    OP_REQUIRES(context, FormatFromString(data_format, &data_format_),
                errors::InvalidArgument("Invalid data format"));

    OP_REQUIRES_OK(context, context->GetAttr("strides", &strides_));
    int stride_n = GetTensorDim(strides_, data_format_, 'N');
    int stride_c = GetTensorDim(strides_, data_format_, 'C');
    OP_REQUIRES(
        context, (stride_n == 1 && stride_c == 1),
        errors::InvalidArgument("Current implementation does not yet support "
                                "strides in the batch and depth dimensions."));
    OP_REQUIRES_OK(context, context->GetAttr("padding", &padding_));
  }

  void Compute(OpKernelContext* context) override {
    MklConv2DGradFilterOpContext mkl_context;
    const Tensor& input = MklGetInput(context, 0);
    GetMklShape(context, 0, &(mkl_context.input_shape));
    bool input_in_mkl_format = mkl_context.input_shape.IsMklTensor();

    const Tensor& filter_sizes = MklGetInput(context, 1);

    const Tensor& out_backprop = MklGetInput(context, 2);
    GetMklShape(context, 2, &(mkl_context.out_backprop_shape));
    bool out_backprop_in_mkl_format =
        mkl_context.out_backprop_shape.IsMklTensor();

    TensorShape input_shape, filter_shape, out_backprop_shape;

    OP_REQUIRES(
        context, TensorShapeUtils::IsVector(filter_sizes.shape()),
        errors::InvalidArgument(
            "Conv2DCustomBackpropFilter: filter_sizes input must be 1-dim, "
            "not ",
            filter_sizes.dims()));
    OP_REQUIRES_OK(context, TensorShapeUtils::MakeShape(
                                filter_sizes.vec<int32>(), &filter_shape));

    ConvBackpropDimensions backprop_dims;

    // Generate shape for input if input is in MKL format.
    if (input_in_mkl_format) {
      OP_REQUIRES(context, mkl_context.input_shape.GetDimension() == 4,
                  errors::InvalidArgument(
                      "Conv2DCustomBackpropFilter: input size must be 4-dim"));

      MklSizesToTFSizes(context, data_format_, mkl_context.input_shape,
                        &input_shape);
    } else {
      input_shape = input.shape();
    }

    // Generate shape for outback prop if input is in MKL format.
    if (out_backprop_in_mkl_format) {
      OP_REQUIRES(
          context, mkl_context.out_backprop_shape.GetDimension() == 4,
          errors::InvalidArgument(
              "Conv2DCustomBackpropFilter: outbackprop size must be 4-dim"));

      MklSizesToTFSizes(context, data_format_, mkl_context.out_backprop_shape,
                        &out_backprop_shape);
    } else {
      out_backprop_shape = out_backprop.shape();
    }

    OP_REQUIRES_OK(context,
                   ConvBackpropComputeDimensions(
                       "Conv2DCustomBackpropFilter", /*num_spatial_dims=*/2,
                       input_shape, filter_shape, out_backprop_shape, strides_,
                       padding_, data_format_, &backprop_dims));

    int64 pad_top, pad_bottom;
    int64 pad_left, pad_right;
    OP_REQUIRES_OK(context, GetWindowedOutputSizeVerbose(
                                backprop_dims.spatial_dims[0].input_size,
                                backprop_dims.spatial_dims[0].filter_size,
                                backprop_dims.spatial_dims[0].stride, padding_,
                                &backprop_dims.spatial_dims[0].output_size,
                                &pad_top, &pad_bottom));
    OP_REQUIRES_OK(context, GetWindowedOutputSizeVerbose(
                                backprop_dims.spatial_dims[1].input_size,
                                backprop_dims.spatial_dims[1].filter_size,
                                backprop_dims.spatial_dims[1].stride, padding_,
                                &backprop_dims.spatial_dims[1].output_size,
                                &pad_left, &pad_right));

    // Create MKL primitives for convolution filter grad
    mkl_context.in_dims = input_in_mkl_format
                              ? mkl_context.input_shape.GetDimension()
                              : input.dims();
    mkl_context.out_dims = out_backprop_in_mkl_format
                               ? mkl_context.out_backprop_shape.GetDimension()
                               : out_backprop.dims();
    mkl_context.in_sizes[0] =
        static_cast<size_t>(backprop_dims.spatial_dims[1].input_size);
    mkl_context.in_sizes[1] =
        static_cast<size_t>(backprop_dims.spatial_dims[0].input_size);
    mkl_context.in_sizes[2] = static_cast<size_t>(backprop_dims.in_depth);
    mkl_context.in_sizes[3] = static_cast<size_t>(backprop_dims.batch_size);
    mkl_context.out_sizes[0] =
        static_cast<size_t>(backprop_dims.spatial_dims[1].output_size);
    mkl_context.out_sizes[1] =
        static_cast<size_t>(backprop_dims.spatial_dims[0].output_size);
    mkl_context.out_sizes[2] = static_cast<size_t>(backprop_dims.out_depth);
    mkl_context.out_sizes[3] = static_cast<size_t>(backprop_dims.batch_size);
    mkl_context.input_offsets[0] = static_cast<int>(-pad_left);
    mkl_context.input_offsets[1] = static_cast<int>(-pad_top);
    mkl_context.conv_strides[0] =
        static_cast<size_t>(backprop_dims.spatial_dims[1].stride);
    mkl_context.conv_strides[1] =
        static_cast<size_t>(backprop_dims.spatial_dims[0].stride);

    GetStridesFromSizes(data_format_, mkl_context.in_strides,
                        mkl_context.in_sizes);
    GetStridesFromSizes(data_format_, mkl_context.out_strides,
                        mkl_context.out_sizes);

    // MKL understands dimensions in 0, 1, 2, and 3 indices denotes
    // filter cols, rows, input channels, and output depth/channels.
    mkl_context.filter_dims = 4;
    mkl_context.filter_sizes[0] = backprop_dims.spatial_dims[1].filter_size;
    mkl_context.filter_sizes[1] = backprop_dims.spatial_dims[0].filter_size;
    mkl_context.filter_sizes[2] = backprop_dims.in_depth;
    mkl_context.filter_sizes[3] = backprop_dims.out_depth;

    // We want filter grad to be in TF format, so
    // make the strides accordingly to reflect this fact.
    // Note TF filter layout : (rows, cols, in_depth, out_depth),
    // while row is the innermost dimension.
    mkl_context.filter_strides[0] =
        backprop_dims.out_depth * backprop_dims.in_depth;
    mkl_context.filter_strides[1] = backprop_dims.out_depth *
                                    backprop_dims.in_depth *
                                    backprop_dims.spatial_dims[1].filter_size;
    mkl_context.filter_strides[2] = backprop_dims.out_depth;
    mkl_context.filter_strides[3] = 1;

    mkl_context.conv_strides[0] = backprop_dims.spatial_dims[1].stride;
    mkl_context.conv_strides[1] = backprop_dims.spatial_dims[0].stride;

    // Create convolution-grad-filter primitive
    CHECK_EQ(dnnConvolutionCreateBackwardFilter_F32(
                 &mkl_context.prim_conv_bwdfilter, nullptr,
                 dnnAlgorithmConvolutionDirect, mkl_context.in_dims,
                 mkl_context.in_sizes, mkl_context.out_sizes,
                 mkl_context.filter_sizes, mkl_context.conv_strides,
                 mkl_context.input_offsets, dnnBorderZeros),
             E_SUCCESS);

    // Create the layouts for entities in received context.
    mkl_context.MklCreateInputLayouts(context);

    // Mkl needs the entities in its native format.
    // So create temporary tensors along with buffers to
    // convert the received entities.
    Tensor mkl_tmp_input_buf_tensor, mkl_tmp_out_backprop_buf_tensor;
    // This preparation sets (1) dnnResourceSrc (2) dnnResourceDiffDst
    mkl_context.MklPrepareInputs(context, &mkl_tmp_input_buf_tensor,
                                 &mkl_tmp_out_backprop_buf_tensor);

    // Final conv-grad-filter should be in TF layout.
    Tensor* grad_filter;
    mkl_context.grad_filter_shape.SetMklTensor(false);
    mkl_context.grad_filter_shape.SetTfLayout(mkl_context.filter_dims,
                                              mkl_context.filter_sizes,
                                              mkl_context.filter_strides);
    AllocateOutputSetMklShape(context, 0, &grad_filter, filter_shape,
                              mkl_context.grad_filter_shape);

    // Need to set member variable for TF layout
    mkl_context.lt_grad_filter = mkl_context.grad_filter_shape.GetTfLayout();

    // MKL conv-grad-filter might produce grad in its internal layout
    Tensor mkl_tmp_grad_filter_buf_tensor;
    // This preparation sets conversion primitive if required
    // and allocates temporary tensor and its buffer without doing conversions.
    // Also sets (3) dnnResourceDiffFilter accordingly
    mkl_context.MklPrepareGradFilter(context, grad_filter,
                                     &mkl_tmp_grad_filter_buf_tensor);

    // After setting all the required dnnResources, ready for execution!
    CHECK_EQ(
        dnnExecute_F32(mkl_context.prim_conv_bwdfilter, mkl_context.conv_res),
        E_SUCCESS);

    // Convert grad-filter to TF layout
    if (mkl_context.convert_bwdfilter != nullptr) {
      void* mkl_buf_convert_grad_filter =
          const_cast<void*>(static_cast<const void*>(
              mkl_tmp_grad_filter_buf_tensor.flat<T>().data()));
      void* mkl_buf_grad_filter = const_cast<void*>(
          static_cast<const void*>(grad_filter->flat<T>().data()));
      CHECK_EQ(dnnConversionExecute_F32(mkl_context.convert_bwdfilter,
                                        mkl_buf_convert_grad_filter,
                                        mkl_buf_grad_filter),
               E_SUCCESS);
    }

    mkl_context.MklCleanup();
  }

 private:
  typedef struct {
    int in_dims;
    size_t in_sizes[4];
    size_t in_strides[4];
    int out_dims;
    size_t out_sizes[4];
    size_t out_strides[4];
    int filter_dims;
    size_t filter_sizes[4];
    size_t filter_strides[4];
    int input_offsets[2];
    size_t conv_strides[2];
    MklShape input_shape, grad_filter_shape, out_backprop_shape;
    dnnPrimitive_t prim_conv_bwdfilter = nullptr;
    dnnPrimitive_t convert_bwdfilter = nullptr;
    dnnLayout_t lt_input = nullptr;
    dnnLayout_t lt_grad_filter = nullptr;
    dnnLayout_t lt_out_backprop = nullptr;
    void* conv_res[dnnResourceNumber];

    void MklCleanup() {
      // Cleanup member layouts and primitives except "lt_grad_filter_"
      // which points to MklShape's TFLayout
      bool input_in_mkl_format = input_shape.IsMklTensor();
      bool out_backprop_in_mkl_format = out_backprop_shape.IsMklTensor();
      if (!input_in_mkl_format) dnnLayoutDelete_F32(lt_input);
      if (!out_backprop_in_mkl_format) dnnLayoutDelete_F32(lt_out_backprop);
      if (convert_bwdfilter != nullptr) dnnDelete_F32(convert_bwdfilter);
      dnnDelete_F32(prim_conv_bwdfilter);
    }

    // Create MKL dnnLayout_t objects for tensors coming into the layer
    void MklCreateInputLayouts(OpKernelContext* context) {
      bool input_in_mkl_format = input_shape.IsMklTensor();
      if (input_in_mkl_format) {
        lt_input = static_cast<dnnLayout_t>(input_shape.GetCurLayout());
      } else {
        CHECK_EQ(dnnLayoutCreate_F32(&lt_input, in_dims, in_sizes, in_strides),
                 E_SUCCESS);
      }

      bool out_backprop_in_mkl_format = out_backprop_shape.IsMklTensor();
      if (out_backprop_in_mkl_format) {
        lt_out_backprop =
            static_cast<dnnLayout_t>(out_backprop_shape.GetCurLayout());
      } else {
        CHECK_EQ(dnnLayoutCreate_F32(&lt_out_backprop, out_dims, out_sizes,
                                     out_strides),
                 E_SUCCESS);
      }
    }

    // Compare incoming tensor layouts with MKL preferred layouts and convert
    // data to the preferred layout if necessary
    void MklPrepareInputs(OpKernelContext* context,
                          Tensor* mkl_tmp_input_buf_tensor,
                          Tensor* mkl_tmp_out_backprop_buf_tensor) {
      bool mkl_convert_input, mkl_convert_out_backprop;
      dnnPrimitive_t mkl_prim_convert_input, mkl_prim_convert_out_backprop;
      dnnLayout_t mkl_lt_internal_input, mkl_lt_internal_out_backprop;
      void *mkl_buf_convert_input, *mkl_buf_convert_out_backprop;

      mkl_prim_convert_input = nullptr;
      mkl_prim_convert_out_backprop = nullptr;
      mkl_lt_internal_input = nullptr;
      mkl_lt_internal_out_backprop = nullptr;
      mkl_buf_convert_input = nullptr;
      mkl_buf_convert_out_backprop = nullptr;

      // Compare with internal layouts and convert if needed
      const Tensor& input = MklGetInput(context, 0);
      void* mkl_buf_input =
          const_cast<void*>(static_cast<const void*>(input.flat<T>().data()));
      CHECK_EQ(dnnLayoutCreateFromPrimitive_F32(
                   &mkl_lt_internal_input, prim_conv_bwdfilter, dnnResourceSrc),
               E_SUCCESS);
      mkl_convert_input =
          !dnnLayoutCompare_F32(mkl_lt_internal_input, lt_input);
      if (mkl_convert_input) {
        CHECK_EQ(dnnConversionCreate_F32(&mkl_prim_convert_input, lt_input,
                                         mkl_lt_internal_input),
                 E_SUCCESS);
        AllocTmpBuffer(context, mkl_tmp_input_buf_tensor, mkl_lt_internal_input,
                       &mkl_buf_convert_input);
        CHECK_EQ(dnnConversionExecute_F32(mkl_prim_convert_input, mkl_buf_input,
                                          mkl_buf_convert_input),
                 E_SUCCESS);
        dnnDelete_F32(mkl_prim_convert_input);
      }
      dnnLayoutDelete_F32(mkl_lt_internal_input);

      conv_res[dnnResourceSrc] =
          (mkl_convert_input) ? mkl_buf_convert_input : mkl_buf_input;

      const Tensor& out_backprop = MklGetInput(context, 2);
      void* mkl_buf_out_backprop = const_cast<void*>(static_cast<const void*>(
                                      out_backprop.flat<T>().data()));

      CHECK_EQ(dnnLayoutCreateFromPrimitive_F32(&mkl_lt_internal_out_backprop,
                                                prim_conv_bwdfilter,
                                                dnnResourceDiffDst),
               E_SUCCESS);
      mkl_convert_out_backprop =
          !dnnLayoutCompare_F32(mkl_lt_internal_out_backprop, lt_out_backprop);
      if (mkl_convert_out_backprop) {
        CHECK_EQ(dnnConversionCreate_F32(&mkl_prim_convert_out_backprop,
                      lt_out_backprop, mkl_lt_internal_out_backprop),
                 E_SUCCESS);
        AllocTmpBuffer(context, mkl_tmp_out_backprop_buf_tensor,
            lt_out_backprop, &mkl_buf_convert_out_backprop);
        CHECK_EQ(dnnConversionExecute_F32(mkl_prim_convert_out_backprop,
                                          mkl_buf_out_backprop,
                                          mkl_buf_convert_out_backprop),
                 E_SUCCESS);
        dnnDelete_F32(mkl_prim_convert_out_backprop);
      }
      dnnLayoutDelete_F32(mkl_lt_internal_out_backprop);

      conv_res[dnnResourceDiffDst] = (mkl_convert_out_backprop)
                                         ? mkl_buf_convert_out_backprop
                                         : mkl_buf_out_backprop;
    }

    void MklPrepareGradFilter(OpKernelContext* context, Tensor* grad_filter,
                              Tensor* mkl_tmp_grad_filter_buf_tensor) {
      bool mkl_convert_grad_filter;
      dnnLayout_t mkl_lt_internal_grad_filter = nullptr;
      void* mkl_buf_convert_grad_filter = nullptr;
      void* mkl_buf_grad_filter = const_cast<void*>(
          static_cast<const void*>(grad_filter->flat<T>().data()));
      CHECK_EQ(dnnLayoutCreateFromPrimitive_F32(&mkl_lt_internal_grad_filter,
                                                prim_conv_bwdfilter,
                                                dnnResourceDiffFilter),
               E_SUCCESS);
      mkl_convert_grad_filter =
          !dnnLayoutCompare_F32(mkl_lt_internal_grad_filter, lt_grad_filter);
      if (mkl_convert_grad_filter) {
        CHECK_EQ(dnnConversionCreate_F32(&convert_bwdfilter,
                                         mkl_lt_internal_grad_filter,
                                         lt_grad_filter),
                 E_SUCCESS);
        AllocTmpBuffer(context, mkl_tmp_grad_filter_buf_tensor,
                       mkl_lt_internal_grad_filter,
                       &mkl_buf_convert_grad_filter);
      }
      dnnLayoutDelete_F32(mkl_lt_internal_grad_filter);

      conv_res[dnnResourceDiffFilter] = (mkl_convert_grad_filter)
                                            ? mkl_buf_convert_grad_filter
                                            : mkl_buf_grad_filter;
    }
  } MklConv2DGradFilterOpContext;

  std::vector<int32> strides_;
  Padding padding_;
  TensorFormat data_format_;
};

#else

template <typename Device, class T>
class MklConv2DCustomBackpropFilterOp : public OpKernel {
 public:
  explicit MklConv2DCustomBackpropFilterOp(OpKernelConstruction* context)
      : OpKernel(context) {
    string data_format;
    OP_REQUIRES_OK(context, context->GetAttr("data_format", &data_format));
    OP_REQUIRES(context, FormatFromString(data_format, &data_format_),
                errors::InvalidArgument("Invalid data format"));

    OP_REQUIRES_OK(context, context->GetAttr("strides", &strides_));
    int stride_n = GetTensorDim(strides_, data_format_, 'N');
    int stride_c = GetTensorDim(strides_, data_format_, 'C');
    OP_REQUIRES(
        context, (stride_n == 1 && stride_c == 1),
        errors::InvalidArgument("Current implementation does not yet support "
                                "strides in the batch and depth dimensions."));
    OP_REQUIRES_OK(context, context->GetAttr("padding", &padding_));
  }

  void Compute(OpKernelContext* context) override {
    try {
      auto cpu_engine = engine(engine::cpu, 0);

      MklDnnData<T> input(&cpu_engine);
      MklDnnData<T> outbackprop(&cpu_engine);
      MklDnnData<T> output(&cpu_engine);

      // Input tensors
      const Tensor& input_tensor = MklGetInput(context, 0);
      const Tensor& filter_tensor = MklGetInput(context, 1);
      const Tensor& obp_tensor = MklGetInput(context, 2);  // Outbackprop

      // Generate input shapes.
      TensorShape filter_shape;
      OP_REQUIRES(context, TensorShapeUtils::IsVector(filter_tensor.shape()),
        errors::InvalidArgument(
              "Conv2DBackpropFilter: filter_sizes input must be 1-dim, not ",
              filter_tensor.dims()));
      OP_REQUIRES_OK(context, TensorShapeUtils::MakeShape(
                        filter_tensor.vec<int32>(), &filter_shape));
      TensorShape input_shape = input_tensor.shape();
      TensorShape obp_shape = obp_tensor.shape();

      // By default, all dims are in MKL order. Only dims in TF order
      // are those with prefix tf_order.
      memory::dims obp_dims, fwd_input_dims, fwd_filter_dims;
      memory::dims padding_l, padding_r, strides, fwd_output_dims;
      memory::dims fwd_output_dims_tf_order;

      // Get forward convolution parameters.
      MklDnnConvUtil conv_utl(context, strides_, padding_, data_format_);
      conv_utl.GetConvFwdSizesInMklOrder(input_shape, filter_shape,
                                         &fwd_input_dims, &fwd_filter_dims,
                                         &strides,
                                         &fwd_output_dims_tf_order,
                                         &fwd_output_dims,
                                         &padding_l, &padding_r);
      if (!context->status().ok()) return;

      // Create Convolution forward descriptor since Convolution backward
      // API needs it. For that, we first need to create input, filter
      // and output memory descriptors.
      auto mkl_data_format = TFDataFormatToMklDnnDataFormat(data_format_);
      auto fwd_src_md = memory::desc(fwd_input_dims, MklDnnType<T>(),
                                     mkl_data_format);
      auto fwd_filter_md = memory::desc(fwd_filter_dims, MklDnnType<T>(),
                                        memory::format::hwio);
      auto fwd_out_md = memory::desc(fwd_output_dims, MklDnnType<T>(),
                                     mkl_data_format);
      auto fwd_desc = convolution_forward::desc(prop_kind::forward,
            convolution_direct, fwd_src_md, fwd_filter_md, fwd_out_md,
            strides, padding_l, padding_r, TFPaddingToMklDnnPadding(padding_));
      auto fwd_pd = convolution_forward::primitive_desc(fwd_desc, cpu_engine);

      // Allocate output tensor and shape
      // TODO(nhasabni): Update this when support for MKL layout is added.
      // Shape of output of Conv2DBackpropInput is same as 'input' of Conv2D.
      TensorShape tf_output_shape(filter_shape);
      MklShape mkl_output_mkl_shape;
      mkl_output_mkl_shape.SetMklTensor(false);
      Tensor* output_tensor = nullptr;
      AllocateOutputSetMklShape(context, 0, &output_tensor, tf_output_shape,
                                mkl_output_mkl_shape);

      // Create memory for user data.
      // Describe how the inputs and outputs of Convolution look like. Also
      // specify buffers containing actual input and output data.
      // Although input shape required is in MKL-DNN order, the layout is
      // Tensorflow's layout (NHWC or NCHW depending on data format).
      input.SetUsrMem(fwd_input_dims, mkl_data_format, &input_tensor);
      // Outbackprop shape is NHWC or NCHW depending on data format. Since
      // GetInputSizeInMklOrder function returns size in that order we just use
      // use that function directly.
      conv_utl.GetInputSizeInMklOrder(obp_shape, &obp_dims);
      if (!context->status().ok()) return;
      outbackprop.SetUsrMem(obp_dims, mkl_data_format, &obp_tensor);
      // Although output shape required is in MKL-DNN order,
      // layout is Tensorflow's filter layout (HWIO)
      // Shape of output of Conv2DBackpropInput is same as shape of filter.
      memory::dims bwd_output_dims = fwd_filter_dims;
      output.SetUsrMem(bwd_output_dims, memory::format::hwio, output_tensor);

      // Create memory descriptors for convolution data w/ no specified format.
      input.SetOpMemDesc(fwd_input_dims, memory::format::any);
      outbackprop.SetOpMemDesc(obp_dims, memory::format::any);
      output.SetOpMemDesc(bwd_output_dims, memory::format::any);

      // Create convolution backward weights primitive.
      auto bwd_desc = convolution_backward_weights::desc(convolution_direct,
                          input.GetOpMemDesc(), output.GetOpMemDesc(),
                          outbackprop.GetOpMemDesc(), strides, padding_l,
                          padding_r, TFPaddingToMklDnnPadding(padding_));

      auto bwd_pd = convolution_backward_weights::primitive_desc(bwd_desc,
                                                              cpu_engine,
                                                              fwd_pd);

      PrepareAndExecutePrimitive(bwd_pd, &input, &outbackprop, &output);
    } catch (mkldnn::error &e) {
     string error_msg = "Status: " + std::to_string(e.status) +
                       ", message: " + string(e.message) +
                       ", in file " + string(__FILE__) + ":" +
                       std::to_string(__LINE__);
     OP_REQUIRES_OK(context, errors::Aborted("Operation received an exception:",
                                            error_msg));
    }
  }

 private:
  std::vector<int32> strides_;
  Padding padding_;
  TensorFormat data_format_;

  // Prepare and execute net - checks for input and output reorders.
  void PrepareAndExecutePrimitive(
                  const convolution_backward_weights::primitive_desc& conv_pd,
                  MklDnnData<T>* input, MklDnnData<T>* obp,
                  MklDnnData<T>* output) {
    // Create reorders between user layout and MKL layout if it is needed and
    // add it to the net before convolution.
    std::vector<primitive> net;
    input->CheckReorderToOpMem(conv_pd.src_primitive_desc(), &net);
    obp->CheckReorderToOpMem(conv_pd.diff_dst_primitive_desc(), &net);

    // Memory for output of convolution. Since we may need reorder on the
    // output side, we will prepare reorder primitive in case output
    // reorder to user memory is required.
    bool output_reorder_required = output->PrepareReorderToUserMemIfReq(
                                      conv_pd.diff_weights_primitive_desc());

    net.push_back(convolution_backward_weights(conv_pd, input->GetOpMem(),
                                    obp->GetOpMem(), output->GetOpMem()));

    // Insert reorder primitive in the net for output reorder if reorder is
    // required.
    if (output_reorder_required) {
      output->InsertReorderToUserMem(&net);
    }

    // Handle output reorder
    stream(stream::kind::eager).submit(net).wait();
  }
};
#endif

#define REGISTER_MKL_FILTER_KERNELS(T)                              \
  REGISTER_KERNEL_BUILDER(Name("_MklConv2DBackpropFilter")          \
                              .Device(DEVICE_CPU)                   \
                              .TypeConstraint<T>("T")               \
                              .Label(mkl_op_registry::kMklOpLabel), \
                          MklConv2DCustomBackpropFilterOp<CPUDevice, T>);

TF_CALL_float(REGISTER_MKL_FILTER_KERNELS);
#undef REGISTER_MKL_FILTER_KERNELS
}  // namespace tensorflow

#endif  // INTEL_MKL
