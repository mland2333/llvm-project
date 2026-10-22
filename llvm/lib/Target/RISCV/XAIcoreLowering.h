//===-- XAIcoreLowering.h - Tile NPU call lowering --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tile NPU（yai-isa v1.0）扩展的调用 lowering。
//
// Clang 把 __builtin_yai_* 先降成 __yai_* 外部调用（Tile 值是 <1024 x i8>
// 的 SSA 值）；RISCVTargetLowering 在调用 lowering 阶段识别这些符号，把它们
// 折叠成 YAI MachineInstr。Tile 值作为 XTT 寄存器操作数交给 LLVM 的寄存器
// 分配器，程序员不接触物理编号。
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RISCV_XAICORELOWERING_H
#define LLVM_LIB_TARGET_RISCV_XAICORELOWERING_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {

class RISCVSubtarget;

/// 把 __yai_* 调用折叠为操作 XTT 寄存器的 YAI 机器指令。
///
/// 返回空 SDValue 表示该调用不是 YAI builtin（或不是直接调用），调用方
/// 应该继续走正常的 RISC-V ABI 调用 lowering。返回非空时，函数已经把
/// 调用结果写入 \p InVals，并返回新的调用链。
SDValue lowerXAIcoreBuiltinCall(const RISCVSubtarget &Subtarget,
                                TargetLowering::CallLoweringInfo &CLI,
                                SmallVectorImpl<SDValue> &InVals);

} // namespace llvm

#endif // LLVM_LIB_TARGET_RISCV_XAICORELOWERING_H
