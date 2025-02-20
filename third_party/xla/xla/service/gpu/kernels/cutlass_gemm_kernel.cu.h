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

#ifndef XLA_SERVICE_GPU_KERNELS_CUTLASS_GEMM_KERNEL_CU_H_
#define XLA_SERVICE_GPU_KERNELS_CUTLASS_GEMM_KERNEL_CU_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "third_party/gpus/cutlass/include/cutlass/cutlass.h"
#include "third_party/gpus/cutlass/include/cutlass/gemm/device/gemm_universal.h"
#include "third_party/gpus/cutlass/include/cutlass/gemm/gemm_enumerated_types.h"
#include "third_party/gpus/cutlass/include/cutlass/gemm_coord.h"
#include "third_party/gpus/cutlass/include/cutlass/layout/matrix.h"
#include "xla/service/gpu/kernels/cutlass_gemm.h"
#include "xla/statusor.h"
#include "xla/stream_executor/kernel.h"
#include "xla/stream_executor/kernel_spec.h"
#include "xla/stream_executor/launch_dim.h"

namespace xla::gpu::kernel::gemm_universal {

// This is a template library that implements an adaptor from a CUTLASS
// GemmUniversal kernel to StreamExecutor primitives for kernel arguments
// packing and kernel launching.
//
// In all templates defined below `typename Gemm` should be a
// an instance of `cutlass::gemm::device::GemmUniversal` template.

namespace se = ::stream_executor;

//===----------------------------------------------------------------------===//
// Gemm launch dimension computation.
//===----------------------------------------------------------------------===//

template <typename Gemm>
se::ThreadDim ThreadDim() {
  using Kernel = typename Gemm::GemmKernel;
  return se::ThreadDim(Kernel::kThreadCount, 1, 1);
}

template <typename Gemm>
se::BlockDim BlockDim(const cutlass::gemm::GemmCoord &problem_size) {
  using ThreadblockSwizzle = typename Gemm::ThreadblockSwizzle;
  using ThreadblockShape = typename Gemm::ThreadblockShape;

  cutlass::gemm::GemmCoord tile_size = {
      ThreadblockShape::kM, ThreadblockShape::kN, ThreadblockShape::kK};

  cutlass::gemm::GemmCoord grid_tiled_shape =
      ThreadblockSwizzle::get_tiled_shape(problem_size, tile_size,
                                          /*split_k_slices=*/1);

  auto grid = ThreadblockSwizzle().get_grid_shape(grid_tiled_shape);

  return se::BlockDim(grid.x, grid.y, grid.z);
}

//===----------------------------------------------------------------------===//
// Gemm strides computation.
//===----------------------------------------------------------------------===//

template <typename Gemm>
int64_t LdA(const cutlass::gemm::GemmCoord &problem_size) {
  using LayoutA = typename Gemm::LayoutA;

  if constexpr (std::is_same_v<LayoutA, cutlass::layout::RowMajor>) {
    return problem_size.k();
  } else {
    static_assert(sizeof(Gemm) == 0, "unsupported layout type");
  }
}

template <typename Gemm>
int64_t LdB(const cutlass::gemm::GemmCoord &problem_size) {
  using LayoutB = typename Gemm::LayoutB;

  if constexpr (std::is_same_v<LayoutB, cutlass::layout::RowMajor>) {
    return problem_size.n();
  } else {
    static_assert(sizeof(Gemm) == 0, "unsupported layout type");
  }
}

template <typename Gemm>
int64_t LdC(const cutlass::gemm::GemmCoord &problem_size) {
  using LayoutC = typename Gemm::LayoutA;

  if constexpr (std::is_same_v<LayoutC, cutlass::layout::RowMajor>) {
    return problem_size.n();
  } else {
    static_assert(sizeof(Gemm) == 0, "unsupported layout type");
  }
}

//===----------------------------------------------------------------------===//
// Packing kernel arguments to CUTLASS kernel parameters struct.
//===----------------------------------------------------------------------===//

using KernelArgsPacking = se::MultiKernelLoaderSpec::KernelArgsPacking;

template <typename Gemm, size_t index>
auto *ArgPtr(const se::KernelArgsDeviceMemoryArray *args,
             const ArgsIndices &indices) {
  if constexpr (index == 0) {
    const void *opaque = args->device_memory_ptr(indices.lhs);
    return static_cast<typename Gemm::ElementA *>(const_cast<void *>(opaque));
  } else if constexpr (index == 1) {
    const void *opaque = args->device_memory_ptr(indices.rhs);
    return static_cast<typename Gemm::ElementB *>(const_cast<void *>(opaque));
  } else if constexpr (index == 2) {
    const void *opaque = args->device_memory_ptr(indices.out);
    return static_cast<typename Gemm::ElementC *>(const_cast<void *>(opaque));
  } else {
    static_assert(sizeof(Gemm) == 0, "illegal Gemm argument index");
  }
}

int32_t *SlicePtr(const se::KernelArgsDeviceMemoryArray *args, int64_t index) {
  const void *opaque = args->device_memory_ptr(index);
  return static_cast<int32_t *>(const_cast<void *>(opaque));
}

template <typename Gemm>
KernelArgsPacking ArgsPacking(cutlass::gemm::GemmCoord problem_size,
                              const ArgsIndices &indices,
                              const DynamicSliceIndices &slices) {
  using Accumulator = typename Gemm::ElementAccumulator;
  using Arguments = typename Gemm::Arguments;
  using Kernel = typename Gemm::GemmKernel;
  using Params = typename Kernel::Params;

  // Sanity check that we do not accidentally get a giant parameters struct.
  static_assert(sizeof(Params) < 512,
                "Params struct size is unexpectedly large");

  using PackedArgs = StatusOr<std::unique_ptr<se::KernelArgsPackedArrayBase>>;

  return [=](const se::KernelLaunchContext &ctx,
             const se::KernelArgs &args) -> PackedArgs {
    auto *mem_args = Cast<se::KernelArgsDeviceMemoryArray>(&args);

    cutlass::Status can_implement = Kernel::can_implement(problem_size);
    if (can_implement != cutlass::Status::kSuccess) {
      return absl::InternalError(absl::StrCat(
          "CUTLASS kernel can not implement gemm for a given problem size",
          ": m=", problem_size.m(), ", n=", problem_size.n(),
          ", k=", problem_size.k()));
    }

    auto lda = LdA<Gemm>(problem_size);
    auto ldb = LdB<Gemm>(problem_size);
    auto ldc = LdC<Gemm>(problem_size);

    auto ptr_a = ArgPtr<Gemm, 0>(mem_args, indices);
    auto ptr_b = ArgPtr<Gemm, 1>(mem_args, indices);
    auto ptr_c = ArgPtr<Gemm, 2>(mem_args, indices);

    auto mode = cutlass::gemm::GemmUniversalMode::kGemm;

    // TODO(ezhulenev): We hardcode parameters for `LinearCombination` epilogue,
    // however `Gemm` template can be compiled with arbitrary epilogues. We have
    // to support custom epilogues in a way that does not leak cutlass types
    // via the public API function signature.
    Accumulator alpha{1.0};
    Accumulator beta{0.0};

    // CUTLASS operation arguments.
    Arguments arguments(mode, problem_size,
                        1,                           // batch
                        {alpha, beta},               // epilogue
                        ptr_a, ptr_b, ptr_c, ptr_c,  // pointers
                        0, 0, 0, 0,                  // batch strides
                        lda, ldb, ldc, ldc           // strides
    );

    // Query kernel API for SM occupancy for the launch dimensions.
    TF_ASSIGN_OR_RETURN(int32_t sm_occupancy,
                        ctx.kernel()->GetMaxOccupiedBlocksPerCore(
                            ctx.threads(), args.number_of_shared_bytes()));

    // TODO(ezhulenev): Get number of SMs from DeviceDescription.

    // Convert CUTLASS operation arguments to a device kernel parameters.
    Params params(arguments, /*device_sms=*/128, sm_occupancy);

    // Optionally set up dynamic slice parameters to allow kernel adjust buffer
    // pointers passed via `params`.
    DynamicSliceParams slice_params;
    if (slices.out.has_value()) {
      slice_params.out = SlicePtr(mem_args, *slices.out);
    }

    return se::PackKernelArgs<Params, DynamicSliceParams>(
        args.number_of_shared_bytes(), params, slice_params);
  };
}

}  // namespace xla::gpu::kernel::gemm_universal

#endif  // XLA_SERVICE_GPU_KERNELS_CUTLASS_GEMM_KERNEL_CU_H_
