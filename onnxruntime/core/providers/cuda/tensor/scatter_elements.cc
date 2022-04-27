// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cuda/tensor/scatter_elements.h"

#include "core/providers/cuda/tensor/scatter_elements_impl.h"
#include "core/providers/cuda/tensor/gather_elements.h"
#include "core/providers/cpu/tensor/utils.h"

namespace onnxruntime {
namespace cuda {

ONNX_OPERATOR_VERSIONED_KERNEL_EX(Scatter, kOnnxDomain, 9, 10, kCudaExecutionProvider,
                                  (*KernelDefBuilder::Create())
                                      .TypeConstraint("T", DataTypeImpl::AllFixedSizeTensorTypes())
                                      .TypeConstraint("Tind",
                                                      std::vector<MLDataType>{DataTypeImpl::GetTensorType<int32_t>(),
                                                                              DataTypeImpl::GetTensorType<int64_t>()}),
                                  ScatterElements);

ONNX_OPERATOR_VERSIONED_KERNEL_EX(ScatterElements, kOnnxDomain, 11, 12, kCudaExecutionProvider,
                                  (*KernelDefBuilder::Create())
                                      .TypeConstraint("T", DataTypeImpl::AllFixedSizeTensorTypes())
                                      .TypeConstraint("Tind",
                                                      std::vector<MLDataType>{DataTypeImpl::GetTensorType<int32_t>(),
                                                                              DataTypeImpl::GetTensorType<int64_t>()}),
                                  ScatterElements);

ONNX_OPERATOR_KERNEL_EX(ScatterElements, kOnnxDomain, 13, kCudaExecutionProvider,
                        (*KernelDefBuilder::Create())
                            .TypeConstraint("T", DataTypeImpl::AllFixedSizeTensorTypes())
                            .TypeConstraint("Tind", std::vector<MLDataType>{DataTypeImpl::GetTensorType<int32_t>(),
                                                                            DataTypeImpl::GetTensorType<int64_t>()}),
                        ScatterElements);

#define CASE_SCATTER_ELEMENTS_IMPL(type)                                                                               \
  case sizeof(type): {                                                                                                 \
    const type* indices_data = reinterpret_cast<const type*>(indices_data_raw);                                        \
    ORT_RETURN_IF_ERROR(ScatterElementsImpl(stream, rank, axis, input_data, input_size, input_dim_along_axis,          \
                                            input_stride_along_axis, masked_input_strides, indices_data, indices_size, \
                                            indices_fdms, updates_data, output_data));                                 \
  } break

template <typename T>
struct ScatterElements::ComputeImpl {
  Status operator()(cudaStream_t stream, const void* input_data_raw, const void* updates_data_raw,
                    const void* indices_data_raw, void* output_data_raw, const int64_t rank, const int64_t axis,
                    const int64_t input_size, const int64_t input_dim_along_axis, const int64_t input_stride_along_axis,
                    const TArray<int64_t>& masked_input_strides, const int64_t indices_size,
                    TArray<fast_divmod>& indices_fdms, const size_t index_element_size) const {
    typedef typename ToCudaType<T>::MappedType CudaT;
    const CudaT* input_data = reinterpret_cast<const CudaT*>(input_data_raw);
    const CudaT* updates_data = reinterpret_cast<const CudaT*>(updates_data_raw);
    CudaT* output_data = reinterpret_cast<CudaT*>(output_data_raw);
    switch (index_element_size) {
      CASE_SCATTER_ELEMENTS_IMPL(int32_t);
      CASE_SCATTER_ELEMENTS_IMPL(int64_t);
      // should not reach here as we validate if the all relevant types are supported in the Compute method
      default:
        ORT_THROW("Unsupported indices element size by the ScatterElements CUDA kernel");
    }

    return Status::OK();
  }
};

Status ScatterElements::ComputeInternal(OpKernelContext* context) const {
  const auto* input_tensor = context->Input<Tensor>(0);
  const auto& input_shape = input_tensor->Shape();
  const int64_t input_size = input_shape.Size();
  const int64_t input_rank = static_cast<int64_t>(input_shape.NumDimensions());
  const int64_t axis = HandleNegativeAxis(axis_, input_rank);

  const auto* indices_tensor = context->Input<Tensor>(1);
  const auto* updates_tensor = context->Input<Tensor>(2);

  if (input_tensor->DataType() != updates_tensor->DataType()) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "data type is different from updates type");
  }

  const auto& indices_shape = indices_tensor->Shape();
  const int64_t indices_size = indices_shape.Size();
  if (indices_shape != updates_tensor->Shape()) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Indices and updates must have the same shape.");
  }

  // Validate input shapes and ranks (invoke the static method in the CPU GatherElements kernel that hosts the shared
  // checks)
  ORT_RETURN_IF_ERROR(onnxruntime::GatherElements::ValidateInputShapes(input_shape, indices_shape, axis));

  auto* output_tensor = context->Output(0, input_shape);
  if (input_size == 0) return Status::OK();

  TensorShapeVector input_shape_vec = input_shape.AsShapeVector();
  TensorShapeVector indices_shape_vec = indices_shape.AsShapeVector();
  int64_t new_axis, new_rank, input_stride_along_axis;
  TArray<int64_t> masked_input_strides;
  TArray<fast_divmod> indices_fdms;
  CoalesceDimensions(input_shape_vec, indices_shape_vec, axis, new_axis, new_rank, input_stride_along_axis,
                     masked_input_strides, indices_fdms);

  // Use element size instead of concrete types so we can specialize less template functions to reduce binary size.
  int dtype = GetElementType(input_tensor->DataType()->Size());
  if (dtype == ONNX_NAMESPACE::TensorProto_DataType_UNDEFINED) {
    ORT_THROW("Unsupported element size by the ScatterElements CUDA kernel");
  }

  utils::MLTypeCallDispatcher<int8_t, MLFloat16, float, double> t_disp(dtype);
  return t_disp.InvokeRet<Status, ComputeImpl>(
      Stream(), input_tensor->DataRaw(), updates_tensor->DataRaw(), indices_tensor->DataRaw(),
      output_tensor->MutableDataRaw(), new_rank, new_axis, input_size, input_shape_vec[new_axis],
      input_stride_along_axis, masked_input_strides, indices_size, indices_fdms, indices_tensor->DataType()->Size());
}

}  // namespace cuda
}  // namespace onnxruntime
