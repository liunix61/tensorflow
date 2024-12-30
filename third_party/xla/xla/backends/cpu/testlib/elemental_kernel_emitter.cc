/* Copyright 2024 The OpenXLA Authors.

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

#include "xla/backends/cpu/testlib/elemental_kernel_emitter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "xla/backends/cpu/codegen/kernel_api_ir_builder.h"
#include "xla/backends/cpu/testlib/llvm_ir_kernel_spec.h"  // Move this outside of testlib?
#include "xla/codegen/kernel_spec.h"
#include "xla/codegen/llvm_ir_kernel_source.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/service/buffer_assignment.h"
#include "xla/service/cpu/backend_config.pb.h"
#include "xla/service/cpu/elemental_ir_emitter.h"
#include "xla/service/cpu/parallel_loop_emitter.h"
#include "xla/service/cpu/shape_partition.h"
#include "xla/service/elemental_ir_emitter.h"
#include "xla/service/llvm_ir/ir_array.h"
#include "xla/service/llvm_ir/llvm_util.h"
#include "xla/service/llvm_ir/loop_emitter.h"
#include "xla/shape.h"
#include "xla/stream_executor/launch_dim.h"
#include "xla/tsl/platform/errors.h"
#include "xla/util.h"
#include "tsl/platform/statusor.h"

namespace xla::cpu {

namespace {

struct ParallelConfig {
  std::vector<int64_t> outer_dimension_partitions;
};

// Parallel partition bounds for parallelized outer dimensions:
//   vector<[i64 lower_bound, i64 upper_bound]>
using ParallelPartitionBounds =
    std::vector<std::pair<llvm::Value*, llvm::Value*>>;

std::optional<ParallelConfig> GetParallelConfig(const HloInstruction* instr) {
  // Check if the instruction is marked for parallel execution.
  auto backend_config = instr->backend_config<BackendConfig>();
  if (!backend_config.ok() ||
      backend_config->outer_dimension_partitions().empty()) {
    return std::nullopt;
  }

  ParallelConfig config;
  config.outer_dimension_partitions.assign(
      backend_config->outer_dimension_partitions().begin(),
      backend_config->outer_dimension_partitions().end());

  return config;
}

ParallelPartitionBounds EmitParallelPartitionBounds(
    llvm::IRBuilderBase& b,
    const KernelApiIrBuilder::KernelPrototype& kernel_prototype,
    const ParallelConfig& parallel_config, const Shape& shape,
    absl::string_view name) {
  ShapePartitionIterator it(shape, parallel_config.outer_dimension_partitions);

  size_t num_parallel_dimensions =
      parallel_config.outer_dimension_partitions.size();

  // Create a constant array of all partition bounds. We will be indexing into
  // this array using block and thread dimension indices passed in a call frame.
  //
  // Type: [#partitions x [#outer_dimensions x [lower_bound, upper_bound]]]
  //
  llvm::ArrayType* dim_bounds_ty = llvm::ArrayType::get(b.getInt64Ty(), 2);
  llvm::ArrayType* partition_bounds_ty =
      llvm::ArrayType::get(dim_bounds_ty, num_parallel_dimensions);
  llvm::ArrayType* parallel_bounds_ty =
      llvm::ArrayType::get(partition_bounds_ty, it.GetTotalPartitionCount());

  // Build a nested array of partition bounds from shape partition iterator.
  std::vector<llvm::Constant*> partition_bounds;
  for (int64_t i = 0; i < it.GetTotalPartitionCount(); ++i) {
    std::vector<llvm::Constant*> dim_counts;
    for (auto [lower, size] : it.GetPartition(i)) {
      dim_counts.push_back(llvm::ConstantArray::get(
          dim_bounds_ty, {b.getInt64(lower), b.getInt64(lower + size)}));
    }
    partition_bounds.push_back(
        llvm::ConstantArray::get(partition_bounds_ty, dim_counts));
  }

  llvm::Constant* parallel_bounds =
      llvm::ConstantArray::get(parallel_bounds_ty, partition_bounds);

  llvm::Module* module = b.GetInsertBlock()->getParent()->getParent();
  llvm::GlobalVariable* parallel_bounds_global = new llvm::GlobalVariable(
      /*M=*/*module,
      /*Ty=*/parallel_bounds_ty,
      /*isConstant=*/true,
      /*Linkage=*/llvm::GlobalValue::PrivateLinkage,
      /*Initializer=*/parallel_bounds,
      /*Name=*/absl::StrCat(name, "_parallel_bounds"));

  // Construct IR to load bounds for all parallel dimensions.
  ParallelPartitionBounds bounds;
  for (size_t i = 0; i < num_parallel_dimensions; ++i) {
    llvm::Value* partition = kernel_prototype.thread_id.x;
    llvm::Value* parallel_dim = b.getInt32(i);

    llvm::Value* lower_gep = b.CreateInBoundsGEP(
        parallel_bounds_ty, parallel_bounds_global,
        {b.getInt32(0), partition, parallel_dim, b.getInt32(0)},
        absl::StrCat("lo_dim_", i, "_gep"));

    llvm::Value* upper_gep = b.CreateInBoundsGEP(
        parallel_bounds_ty, parallel_bounds_global,
        {b.getInt32(0), partition, parallel_dim, b.getInt32(1)},
        absl::StrCat("up_dim_", i, "_gep"));

    bounds.emplace_back(
        b.CreateLoad(b.getInt64Ty(), lower_gep, absl::StrCat("lo_dim_", i)),
        b.CreateLoad(b.getInt64Ty(), upper_gep, absl::StrCat("up_dim_", i)));
  }

  return bounds;
}

}  // namespace

ElementalKernelEmitter::ElementalKernelEmitter(
    std::unique_ptr<HloInstruction> op_hlo)
    : op_hlo_(std::move(op_hlo)),
      context_(std::make_unique<llvm::LLVMContext>()),
      kernel_api_ir_builder_(*context_.getContext(),
                             KernelApiIrBuilder::Options{true, 256}) {}

absl::StatusOr<std::unique_ptr<KernelSpec>>
ElementalKernelEmitter::EmitKernelSpec() {
  VLOG(2) << "Emit elemental host kernel: " << op_hlo_->name();

  llvm::LLVMContext& ctx = *context_.getContext();
  auto module = std::make_unique<llvm::Module>(
      absl::StrCat(op_hlo_->name(), "_elemental_kernel_module"), ctx);

  TF_ASSIGN_OR_RETURN(
      KernelApiIrBuilder::KernelPrototype kernel_prototype,
      kernel_api_ir_builder_.EmitKernelPrototype(
          *module, op_hlo_.get(), buffer_assignment_, "_kernel"));

  llvm::IRBuilder<> ir_builder(ctx);
  ir_builder.SetInsertPoint(
      kernel_prototype.function->getEntryBlock().getTerminator());

  CpuElementalIrEmitter::HloToElementGeneratorMap operand_to_generator;
  for (int64_t i = 0; i < op_hlo_->operand_count(); ++i) {
    const HloInstruction* operand = op_hlo_->operand(i);
    operand_to_generator[operand] = [&, i](const llvm_ir::IrArray::Index& idx) {
      return kernel_prototype.arguments[i].EmitReadArrayElement(idx,
                                                                &ir_builder);
    };
  }

  CpuElementalIrEmitter elemental_ir_emitter(module.get(), &ir_builder, nullptr,
                                             true, true);

  llvm_ir::ElementGenerator element_generator =
      elemental_ir_emitter.MakeElementGenerator(op_hlo_.get(),
                                                operand_to_generator);

  TF_ASSIGN_OR_RETURN(se::ThreadDim thread_dims,
                      EmitElementalLoops(ir_builder, op_hlo_.get(),
                                         kernel_prototype, element_generator));

  auto source = std::make_unique<LlvmIrKernelSource>(
      context_, std::move(module),
      std::string(kernel_prototype.function->getName()));

  // TODO(willfroom): fill in buffer allocations and buffer uses when we support
  // creation from a real HLO instruction.
  std::vector<BufferAllocation> buffer_allocations;
  KernelSpec::BufferUses buffer_uses;

  return std::make_unique<LlvmIrKernelSpec>(
      thread_dims, std::move(buffer_allocations), std::move(buffer_uses),
      std::move(source));
}

absl::StatusOr<se::ThreadDim> ElementalKernelEmitter::EmitElementalLoops(
    llvm::IRBuilderBase& b, const HloInstruction* instr,
    const KernelApiIrBuilder::KernelPrototype& kernel_prototype,
    const llvm_ir::ElementGenerator& element_generator) {
  // We can emit loops for instruction with multiple results only if it is a
  // fusion, reduce or reduce window.
  bool multiple_results = kernel_prototype.results.size() > 1;
  bool support_multiple_results = instr->opcode() == HloOpcode::kFusion ||
                                  instr->opcode() == HloOpcode::kReduce ||
                                  instr->opcode() == HloOpcode::kReduceWindow;

  auto parallel_config = GetParallelConfig(instr);
  bool has_parallel_config = parallel_config.has_value();

  if (multiple_results && !support_multiple_results) {
    return Internal(
        "Multi-output host kernels are not supported for %s instruction",
        HloOpcodeString(instr->opcode()));
  }

  // TODO(ezhulenev): Support multiple results for parallel loops.
  if (multiple_results) {
    TF_RETURN_IF_ERROR(
        llvm_ir::LoopEmitter(element_generator, kernel_prototype.results, &b)
            .EmitLoop(llvm_ir::IrName(instr)));
    return se::ThreadDim();
  }

  const llvm_ir::IrArray& result = kernel_prototype.results.front();

  // Emit a loop for a single parallel partition with dynamic bounds computed
  // from thread index.
  if (has_parallel_config) {
    ParallelPartitionBounds parallel_bounds = EmitParallelPartitionBounds(
        b, kernel_prototype, *parallel_config, instr->shape(), instr->name());
    TF_RETURN_IF_ERROR(
        ParallelLoopEmitter(element_generator, result, &parallel_bounds, &b)
            .EmitLoop(llvm_ir::IrName(instr)));
    return se::ThreadDim(ShapePartitionAssigner::GetTotalPartitionCount(
        parallel_config->outer_dimension_partitions));
  }

  // Emit a whole loop for the instruction.
  TF_RETURN_IF_ERROR(llvm_ir::LoopEmitter(element_generator, result, &b)
                         .EmitLoop(llvm_ir::IrName(instr)));
  return se::ThreadDim();
}

}  // namespace xla::cpu
