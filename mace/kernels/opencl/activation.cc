// Copyright 2018 Xiaomi, Inc.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mace/kernels/activation.h"
#include "mace/core/runtime/opencl/cl2_header.h"
#include "mace/core/runtime/opencl/opencl_runtime.h"
#include "mace/kernels/opencl/helper.h"
#include "mace/utils/tuner.h"
#include "mace/utils/utils.h"

namespace mace {
namespace kernels {
template <typename T>
MaceStatus ActivationFunctor<DeviceType::GPU, T>::operator()(
    const Tensor *input,
    const Tensor *alpha,
    Tensor *output,
    StatsFuture *future) {
  const index_t batch = input->dim(0);
  const index_t height = input->dim(1);
  const index_t width = input->dim(2);
  const index_t channels = input->dim(3);

  const index_t channel_blocks = RoundUpDiv4(channels);

  auto runtime = OpenCLRuntime::Global();

  if (kernel_.get() == nullptr) {
    std::set<std::string> built_options;
    std::string kernel_name = MACE_OBFUSCATE_SYMBOL("activation");
    built_options.emplace("-Dactivation=" + kernel_name);
    auto dt = DataTypeToEnum<T>::value;
    built_options.emplace("-DDATA_TYPE=" + DtToUpstreamCLDt(dt));
    built_options.emplace("-DCMD_DATA_TYPE=" + DtToUpstreamCLCMDDt(dt));
    if (runtime->IsOutOfRangeCheckEnabled()) {
      built_options.emplace("-DOUT_OF_RANGE_CHECK");
      kernel_error_ = std::move(std::unique_ptr<Buffer>(
          new Buffer(GetDeviceAllocator(DeviceType::GPU))));
      MACE_RETURN_IF_ERROR(kernel_error_->Allocate(1));
      kernel_error_->Map(nullptr);
      *(kernel_error_->mutable_data<char>()) = 0;
      kernel_error_->UnMap();
    }
    if (runtime->IsNonUniformWorkgroupsSupported()) {
      built_options.emplace("-DNON_UNIFORM_WORK_GROUP");
    }
    switch (activation_) {
      case RELU:
        tuning_key_prefix_ = "relu_opencl_kernel";
        built_options.emplace("-DUSE_RELU");
        break;
      case RELUX:
        tuning_key_prefix_ = "relux_opencl_kernel";
        built_options.emplace("-DUSE_RELUX");
        break;
      case PRELU:
        tuning_key_prefix_ = "prelu_opencl_kernel";
        built_options.emplace("-DUSE_PRELU");
        break;
      case TANH:
        tuning_key_prefix_ = "tanh_opencl_kernel";
        built_options.emplace("-DUSE_TANH");
        break;
      case SIGMOID:
        tuning_key_prefix_ = "sigmoid_opencl_kernel";
        built_options.emplace("-DUSE_SIGMOID");
        break;
      default:
        LOG(FATAL) << "Unknown activation type: " << activation_;
    }
    MACE_RETURN_IF_ERROR(runtime->BuildKernel("activation", kernel_name,
                                              built_options, &kernel_));

    kwg_size_ =
        static_cast<uint32_t>(runtime->GetKernelMaxWorkGroupSize(kernel_));
  }

  const uint32_t gws[3] = {static_cast<uint32_t>(channel_blocks),
                           static_cast<uint32_t>(width),
                           static_cast<uint32_t>(height * batch)};

  if (!IsVecEqual(input_shape_, input->shape())) {
    int idx = 0;
    if (runtime->IsOutOfRangeCheckEnabled()) {
      kernel_.setArg(idx++,
                     *(static_cast<cl::Buffer *>(kernel_error_->buffer())));
    }
    if (!runtime->IsNonUniformWorkgroupsSupported()) {
      kernel_.setArg(idx++, gws[0]);
      kernel_.setArg(idx++, gws[1]);
      kernel_.setArg(idx++, gws[2]);
    }
    kernel_.setArg(idx++, *(input->opencl_image()));
    if (activation_ == PRELU) {
      MACE_CHECK_NOTNULL(alpha);
      kernel_.setArg(idx++, *(alpha->opencl_image()));
    }
    kernel_.setArg(idx++, static_cast<float>(relux_max_limit_));
    kernel_.setArg(idx++, *(output->opencl_image()));

    input_shape_ = input->shape();
  }

  const std::vector<uint32_t> lws = Default3DLocalWS(gws, kwg_size_);
  std::string tuning_key =
      Concat(tuning_key_prefix_, output->dim(0), output->dim(1), output->dim(2),
             output->dim(3));
  MACE_RETURN_IF_ERROR(TuningOrRun3DKernel(kernel_, tuning_key, gws,
                                           lws, future));

  if (runtime->IsOutOfRangeCheckEnabled()) {
    kernel_error_->Map(nullptr);
    char *kerror_code = kernel_error_->mutable_data<char>();
    MACE_CHECK(*kerror_code == 0) << "Kernel error code: " << *kerror_code;
    kernel_error_->UnMap();
  }

  return MACE_SUCCESS;
}

template struct ActivationFunctor<DeviceType::GPU, float>;
template struct ActivationFunctor<DeviceType::GPU, half>;
}  // namespace kernels
}  // namespace mace
