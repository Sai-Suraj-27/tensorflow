/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/service/gpu/kernels/cutlass_gemm_custom_kernel.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "xla/stream_executor/kernel.h"
#include "xla/stream_executor/multi_platform_manager.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/stream.h"
#include "xla/stream_executor/stream_executor.h"
#include "xla/xla_data.pb.h"
#include "tsl/lib/core/status_test_util.h"
#include "tsl/platform/test.h"

namespace xla::gpu::kernel::gemm_universal {

TEST(CutlassGemmKernelTest, SimpleGemm) {
  se::Platform* platform =
      se::MultiPlatformManager::PlatformWithName("CUDA").value();
  se::StreamExecutor* executor = platform->ExecutorForDevice(0).value();

  se::Stream stream(executor);
  stream.Init();
  ASSERT_TRUE(stream.ok());

  se::Kernel gemm(executor);

  // Load [4, 4] x [4, 4] gemm kernel written in CUDA C++ with CUTLASS.
  auto custom_kernel =
      GetCutlassGemmKernel("cutlass_gemm", PrimitiveType::F32, 4, 4, 4,
                           /*indices=*/{0, 1, 2}, /*slices=*/{});
  TF_ASSERT_OK(executor->GetKernel(custom_kernel->kernel_spec(), &gemm));

  int64_t length = 4 * 4;
  int64_t byte_length = sizeof(float) * length;

  // Prepare arguments: a=2, b=2, c=0
  se::DeviceMemory<float> a = executor->AllocateArray<float>(length, 0);
  se::DeviceMemory<float> b = executor->AllocateArray<float>(length, 0);
  se::DeviceMemory<float> c = executor->AllocateArray<float>(length, 0);

  float value = 2.0;
  uint32_t pattern;
  std::memcpy(&pattern, &value, sizeof(pattern));

  stream.ThenMemset32(&a, pattern, byte_length);
  stream.ThenMemset32(&b, pattern, byte_length);
  stream.ThenMemZero(&c, byte_length);

  // Launch gemm kernel with device memory arguments.
  se::KernelArgsDeviceMemoryArray arr(
      std::vector<se::DeviceMemoryBase>({a, b, c}),
      custom_kernel->shared_memory_bytes());
  TF_ASSERT_OK(executor->Launch(&stream, custom_kernel->thread_dims(),
                                custom_kernel->block_dims(), gemm, arr));

  // Copy `c` data back to host.
  std::vector<float> dst(length, -1.0f);
  stream.ThenMemcpy(dst.data(), c, byte_length);

  std::vector<float> expected(length, 16.0);
  ASSERT_EQ(dst, expected);
}

}  // namespace xla::gpu::kernel::gemm_universal
