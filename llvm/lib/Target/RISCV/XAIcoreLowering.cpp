//===-- XAIcoreLowering.cpp - Tile NPU call lowering ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// 把 __yai_* 调用折叠成 YAI 机器指令。
//
// Tile/Acc/Vec 都是 LLVM 的 SSA 值，由各自的寄存器类承载：
//
//   Tile  = <1024 x i8>  -> XTT0..XTT15
//   Acc   = <1024 x i32> -> XTA0..XTA15
//   Vec32 = <32 x i32>   -> XTV0..XTV31
//   Vec8  = <32 x i8>    -> XTB0..XTB31
//
// 覆盖的调用：
//
//   tile_create / acc_create / vec32_create / vec8_create
//                              -> *_Create 伪指令，定义一个寄存器值
//   tile_set_* / tile_load / tile_store / tile_get_valid_zone
//                              -> cfg.setx / tload / tstore / cfg.get（Tile）
//   acc_set_* / acc_load / acc_store
//                              -> cfg.setx ac/am、aload、astore
//   tile_matmul(right, acc, zero, lt, rt)
//                              -> mma.nn.{zero,acc}.i8.i32（只支持非转置）
//
// 其余运算（acc 二元/归约/量化、vec 运算、转置 matmul 等）留到后续阶段，
// 遇到时直接给出编译错误。
//
//===----------------------------------------------------------------------===//

#include "XAIcoreLowering.h"
#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "RISCVSubtarget.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/Support/ErrorHandling.h"

#include <cstring>

#include <limits>

using namespace llvm;

namespace {

/// 统一的诊断入口。
[[noreturn]] void failXAIcore(StringRef BuiltinName, const Twine &Message) {
  report_fatal_error(Twine("YAI builtin ") + BuiltinName + ": " + Message);
}

} // namespace

SDValue llvm::lowerXAIcoreBuiltinCall(const RISCVSubtarget &Subtarget,
                                      TargetLowering::CallLoweringInfo &CLI,
                                      SmallVectorImpl<SDValue> &InVals) {
  SelectionDAG &DAG = CLI.DAG;
  const SDLoc &DL = CLI.DL;
  SDValue Chain = CLI.Chain;

  auto *Callee = dyn_cast<GlobalAddressSDNode>(CLI.Callee);
  if (!Callee)
    return SDValue();

  StringRef Name = Callee->getGlobal()->getName();
  if (!Name.starts_with("__yai_"))
    return SDValue();

  if (!Subtarget.is64Bit())
    failXAIcore(Name, "the XAIcore lowering only supports RV64");

  const MVT XLenVT = Subtarget.getXLenVT();
  const MVT TileVT = MVT::v1024i8;
  const MVT AccVT = MVT::v1024i32;
  const MVT Vec32VT = MVT::v32i32;
  const MVT Vec8VT = MVT::v32i8;
  ArrayRef<SDValue> Args = CLI.OutVals;

  auto expectArgs = [&](size_t NumArgs) {
    if (Args.size() != NumArgs)
      failXAIcore(Name, "expected " + Twine(NumArgs) + " arguments, got " +
                            Twine(Args.size()));
  };

  auto requireValue = [&](unsigned Index, MVT VT) {
    if (Index >= Args.size() || Args[Index].getValueType() != VT)
      failXAIcore(Name, "expected a YAI value of the expected register class "
                        "as argument " + Twine(Index));
  };

  // 把标量实参提升到 XLEN：i32/i16 按 C 语义符号扩展，i8/i1（uint8_t、
  // bool）零扩展；base_addr 在 RV64 上本来就是 64-bit 指针。
  auto scalar = [&](unsigned Index) -> SDValue {
    SDValue Value = Args[Index];
    EVT VT = Value.getValueType();
    if (!VT.isInteger())
      failXAIcore(Name, "expected an integer scalar argument");
    if (VT == XLenVT)
      return Value;
    if (VT == MVT::i32 || VT == MVT::i16)
      return DAG.getSExtOrTrunc(Value, DL, XLenVT);
    return DAG.getZExtOrTrunc(Value, DL, XLenVT);
  };

  // 浮点标量（float）先 bitcast 成 i32，再零扩展到 XLEN：指令的 xS 操作数
  // 读 GPR 的低 32 位作为 f32 位模式（fillx / vaddx / vmulx 等）。
  auto fp32Operand = [&](unsigned Index) -> SDValue {
    SDValue Value = Args[Index];
    if (Value.getValueType() == MVT::f32) {
      SDValue Bits = DAG.getNode(ISD::BITCAST, DL, MVT::i32, Value);
      return DAG.getZExtOrTrunc(Bits, DL, XLenVT);
    }
    return scalar(Index);
  };

  // 字段选择器/dim/布尔标志必须是编译期常量。
  auto constant = [&](unsigned Index) -> uint64_t {
    auto *C = dyn_cast<ConstantSDNode>(Args[Index]);
    if (!C)
      failXAIcore(Name, "the field selector must be a compile-time constant");
    return C->getZExtValue();
  };

  // 发射一条 YAI 机器指令，并把 Chain 推进到该指令之后。
  auto emit = [&](unsigned Opcode, ArrayRef<SDValue> Ops) {
    // 只允许"没有 def"的指令走这条路（我们的 in-place 指令把目标寄存器当输入，
    // 所以它们都是 NumDefs == 0）。有 def 的指令必须用 emitDef，否则
    // InstrEmitter 的 optional-def 逻辑会让 MI 的操作数比描述符少一个，
    // 而 MachineLICM 之类按 getDesc().getNumOperands() 遍历的 pass 会越界
    // （__yai_vec32_copy 曾经就是这样）。
    assert(Subtarget.getInstrInfo()->get(Opcode).getNumDefs() == 0 &&
           "instructions with a def must be emitted through emitDef()");
    SmallVector<SDValue, 3> NodeOps(Ops.begin(), Ops.end());
    NodeOps.push_back(Chain);
    SDNode *Node = DAG.getMachineNode(Opcode, DL, MVT::Other, NodeOps);
    Chain = SDValue(Node, 0);
  };

  // 注意：有 def 的指令必须用下面的 emitDef（把 def 声明成 DAG 结果），
  // 不能用 emit（把 def 当操作数）——否则 InstrEmitter 的 optional-def 逻辑
  // 会让生成的 MachineInstr 操作数比描述符少，MachineLICM 按描述符遍历时越界。
  // 定义一个寄存器值（*_create）。带 chain 让每次 create 都是独立节点，
  // 避免 SelectionDAG 对相同操作数的机器节点做 CSE。
  auto defineValue = [&](unsigned Pseudo, MVT VT) -> SDValue {
    SDNode *Node = DAG.getMachineNode(Pseudo, DL, DAG.getVTList(VT, MVT::Other),
                                      Chain);
    Chain = SDValue(Node, 1);
    return SDValue(Node, 0);
  };

  // 归约的“合并”形式：临时 vec（克隆配置）← reduce(acc)，再 vec = vec Op 临时。
  // 归约指令是覆盖写的，所以不能 1:1 映射。
  auto emitReduceMerge = [&](unsigned ReduceOpc, unsigned MergeOpc) {
    SDValue Temp = defineValue(RISCV::XAIcoreVec32Create, Vec32VT);
    emit(RISCV::XAIcoreCfgCopyVC, {Temp, Args[1]});
    emit(ReduceOpc, {Temp, Args[0]});
    emit(MergeOpc, {Args[1], Args[1], Temp});
  };

  // 带操作数的“产生新值”机器节点（twiden/vcopy 这类 def 形态）。
  auto emitDef = [&](unsigned Opcode, MVT VT, ArrayRef<SDValue> Ops) -> SDValue {
    assert(Subtarget.getInstrInfo()->get(Opcode).getNumDefs() == 1 &&
           "emitDef() expects an instruction with exactly one def");
    SmallVector<SDValue, 3> NodeOps(Ops.begin(), Ops.end());
    NodeOps.push_back(Chain);
    SDNode *Node =
        DAG.getMachineNode(Opcode, DL, DAG.getVTList(VT, MVT::Other), NodeOps);
    Chain = SDValue(Node, 1);
    return SDValue(Node, 0);
  };

  // 通用 cfg.get（tc/ac 的 rows/cols）。
  auto emitCfgGet = [&](unsigned Opcode, SDValue Src) -> SDValue {
    SDNode *Node = DAG.getMachineNode(Opcode, DL,
                                     DAG.getVTList(XLenVT, MVT::Other),
                                     {Src, Chain});
    Chain = SDValue(Node, 1);
    return SDValue(Node, 0);
  };

  // 取 ac.rows（归约产生的新 Vec32 需要 rows 作为 vc.len）。
  auto emitCfgGetACRows = [&](SDValue Acc) -> SDValue {
    SDNode *Node =
        DAG.getMachineNode(RISCV::XAIcoreCfgGetACRows, DL,
                           DAG.getVTList(XLenVT, MVT::Other), {Acc, Chain});
    Chain = SDValue(Node, 1);
    return SDValue(Node, 0);
  };

  // 产生新 Vec32 的归约（row_max/row_sum/row_sum_sq/row_absmax）：
  // 新寄存器必须先把 vc.dtype=f32、vc.len=ac.rows 配好，areduce 才能执行。
  auto emitReduceDef = [&](unsigned ReduceOpc, uint64_t DType = 4) -> SDValue {
    SDValue Rows = emitCfgGetACRows(Args[0]);
    SDValue Temp = defineValue(RISCV::XAIcoreVec32Create, Vec32VT);
    emit(RISCV::XAIcoreCfgSetxVCDtype, {Temp, DAG.getConstant(DType, DL, XLenVT)});
    emit(RISCV::XAIcoreCfgSetxVCLen, {Temp, Rows});
    emit(ReduceOpc, {Temp, Args[0]});
    return Temp;
  };

  auto expectResult = [&](MVT VT) {
    if (CLI.Ins.size() != 1 || CLI.Ins[0].VT != VT)
      failXAIcore(Name, "expected a single YAI value result");
  };
  auto expectXLenResult = [&]() {
    if (CLI.Ins.size() != 1 || CLI.Ins[0].VT != XLenVT)
      failXAIcore(Name, "expected a single XLEN result");
  };

  CLI.IsTailCall = false;

  enum FixedCall {
    CallUnsupported = 0,
    // create
    CallTileCreate,
    CallAccCreate,
    CallVec32Create,
    CallVec8Create,
    // Tile
    CallTileSetData,
    CallTileSetStrides,
    CallTileSetStride,
    CallTileSetDType,
    CallTileSetFlags,
    CallTileSetValidZonePair,
    CallTileSetValidZone,
    CallTileSetTranspose,
    CallTileLoad,
    CallTileStore,
    CallTileGetValidZone,
    // Acc
    CallAccSetData,
    CallAccSetStrides,
    CallAccSetStride,
    CallAccSetDType,
    CallAccSetFlags,
    CallAccSetValidZonePair,
    CallAccSetValidZone,
    CallAccLoad,
    CallAccStore,
    CallAccAdd,
    CallAccSub,
    CallAccMul,
    CallAccDiv,
    // Vec32
    CallVecFill,
    CallVecAddScalar,
    CallVecMulScalar,
    CallVecRsqrt,
    CallVecSetDType,
    CallVecSetFlags,
    CallVec8SetFlags,
    CallVecSetValidZone,
    CallVecSetStride,
    CallVecSetData,
    CallVecLoad,
    CallVecStore,
    CallTileGather,
    CallVecGather,
    CallTileWiden,
    CallVecCopyDef,
    CallVecAdd,
    CallVecSub,
    CallVecMul,
    CallVecDiv,
    CallVecMax,
    CallVecExp,
    // 归约 / 广播 / 标量 / 一元
    CallAccRowSumSq,
    CallAccRowSum,
    CallAccRowMax,
    CallAccRowAbsMax,
    CallAccMulVec,
    CallAccAddVec,
    CallAccSubVec,
    CallAccDivVec,
    CallAccAddScalar,
    CallAccMulScalar,
    CallAccFill,
    CallAccExp,
    CallAccSilu,
    CallAccAbs,
    CallAccToFp32,
    CallAccQuant,
    CallAccQuantExt,
    CallAccRowSumDef,
    CallAccRowMaxDef,
    CallAccRowSumSqDef,
    CallAccRowAbsMaxDef,
    CallAccRowArgMaxDef,
    CallAccMask,
    // Matmul
    CallMatmul,
  };

  FixedCall Call =
      StringSwitch<FixedCall>(Name)
          .Case("__yai_tile_create", CallTileCreate)
          .Case("__yai_acc_create", CallAccCreate)
          .Case("__yai_vec32_create", CallVec32Create)
          .Case("__yai_vec8_create", CallVec8Create)
          .Case("__yai_tile_set_data", CallTileSetData)
          .Case("__yai_tile_set_base", CallTileSetData)
          .Case("__yai_tile_set_strides", CallTileSetStrides)
          .Case("__yai_tile_set_stride", CallTileSetStride)
          .Case("__yai_tile_set_dtype", CallTileSetDType)
          .Case("__yai_tile_set_flags", CallTileSetFlags)
          .Case("__yai_tile_set_valid_zone_pair", CallTileSetValidZonePair)
          .Case("__yai_tile_set_valid_zone", CallTileSetValidZone)
          .Case("__yai_tile_set_transpose", CallTileSetTranspose)
          .Case("__yai_tile_load", CallTileLoad)
          .Case("__yai_tile_store", CallTileStore)
          .Case("__yai_tile_get_valid_zone", CallTileGetValidZone)
          .Case("__yai_acc_set_data", CallAccSetData)
          .Case("__yai_acc_set_base", CallAccSetData)
          .Case("__yai_acc_set_strides", CallAccSetStrides)
          .Case("__yai_acc_set_stride", CallAccSetStride)
          .Case("__yai_acc_set_dtype", CallAccSetDType)
          .Case("__yai_acc_set_flags", CallAccSetFlags)
          .Case("__yai_acc_set_valid_zone_pair", CallAccSetValidZonePair)
          .Case("__yai_acc_set_valid_zone", CallAccSetValidZone)
          .Case("__yai_acc_load", CallAccLoad)
          .Case("__yai_acc_store", CallAccStore)
          .Case("__yai_acc_add_inplace", CallAccAdd)
          .Case("__yai_acc_sub_inplace", CallAccSub)
          .Case("__yai_acc_mul_inplace", CallAccMul)
          .Case("__yai_acc_div_inplace", CallAccDiv)
          .Case("__yai_vec32_fill", CallVecFill)
          .Case("__yai_vec32_add_scalar", CallVecAddScalar)
          .Case("__yai_vec32_mul_scalar", CallVecMulScalar)
          .Case("__yai_vec32_rsqrt", CallVecRsqrt)
          .Case("__yai_vec32_set_dtype", CallVecSetDType)
          .Case("__yai_vec32_set_flags", CallVecSetFlags)
          .Case("__yai_vec8_set_flags", CallVec8SetFlags)
          .Case("__yai_vec32_set_valid_zone", CallVecSetValidZone)
          .Case("__yai_vec32_set_stride", CallVecSetStride)
          .Case("__yai_vec32_set_data", CallVecSetData)
          .Case("__yai_vec32_load", CallVecLoad)
          .Case("__yai_vec32_store", CallVecStore)
          .Case("__yai_tile_gather_from_vec", CallTileGather)
          .Case("__yai_vec32_gather_from_vec", CallVecGather)
          .Case("__yai_tile_widen", CallTileWiden)
          .Case("__yai_vec32_copy", CallVecCopyDef)
          .Case("__yai_acc_row_sum_sq_merge", CallAccRowSumSq)
          .Case("__yai_acc_row_sum_merge", CallAccRowSum)
          .Case("__yai_acc_row_max_merge", CallAccRowMax)
          .Case("__yai_acc_row_absmax_merge", CallAccRowAbsMax)
          .Case("__yai_acc_mul_vec", CallAccMulVec)
          .Case("__yai_acc_add_vec", CallAccAddVec)
          .Case("__yai_acc_sub_vec", CallAccSubVec)
          .Case("__yai_acc_div_vec", CallAccDivVec)
          .Case("__yai_acc_add_scalar", CallAccAddScalar)
          .Case("__yai_acc_mul_scalar", CallAccMulScalar)
          .Case("__yai_acc_fill", CallAccFill)
          .Case("__yai_acc_exp", CallAccExp)
          .Case("__yai_acc_silu", CallAccSilu)
          .Case("__yai_acc_abs", CallAccAbs)
          .Case("__yai_acc_to_fp32", CallAccToFp32)
          .Case("__yai_acc_quant", CallAccQuant)
          .Case("__yai_acc_quant_ext", CallAccQuantExt)
          .Case("__yai_acc_row_sum", CallAccRowSumDef)
          .Case("__yai_acc_row_max", CallAccRowMaxDef)
          .Case("__yai_acc_row_sum_sq", CallAccRowSumSqDef)
          .Case("__yai_acc_row_absmax", CallAccRowAbsMaxDef)
          .Case("__yai_acc_row_argmax", CallAccRowArgMaxDef)
          .Case("__yai_acc_mask", CallAccMask)
          .Case("__yai_vec32_add", CallVecAdd)
          .Case("__yai_vec32_sub", CallVecSub)
          .Case("__yai_vec32_mul", CallVecMul)
          .Case("__yai_vec32_div", CallVecDiv)
          .Case("__yai_vec32_max", CallVecMax)
          .Case("__yai_vec32_exp", CallVecExp)
          .Case("__yai_tile_matmul", CallMatmul)
          .Default(CallUnsupported);
  if (Call == CallUnsupported)
    failXAIcore(Name, "not supported by the XAIcore lowering yet");

  switch (Call) {
  //===--------------------------------------------------------------------===//
  // create
  //===--------------------------------------------------------------------===//
  case CallTileCreate:
    expectArgs(0);
    expectResult(TileVT);
    InVals.push_back(defineValue(RISCV::XAIcoreTCreate, TileVT));
    return Chain;
  case CallAccCreate:
    expectArgs(0);
    expectResult(AccVT);
    InVals.push_back(defineValue(RISCV::XAIcoreACreate, AccVT));
    return Chain;
  case CallVec32Create:
    expectArgs(0);
    expectResult(Vec32VT);
    InVals.push_back(defineValue(RISCV::XAIcoreVec32Create, Vec32VT));
    return Chain;
  case CallVec8Create:
    expectArgs(0);
    expectResult(Vec8VT);
    InVals.push_back(defineValue(RISCV::XAIcoreVec8Create, Vec8VT));
    return Chain;

  //===--------------------------------------------------------------------===//
  // Tile
  //===--------------------------------------------------------------------===//
  case CallTileSetData: {
    expectArgs(2);
    requireValue(0, TileVT);
    emit(RISCV::XAIcoreCfgSetxTMBaseAddr, {Args[0], scalar(1)});
    break;
  }
  case CallTileSetStrides: {
    expectArgs(3);
    requireValue(0, TileVT);
    emit(RISCV::XAIcoreCfgSetxTMRowStride, {Args[0], scalar(1)});
    emit(RISCV::XAIcoreCfgSetxTMColStride, {Args[0], scalar(2)});
    break;
  }
  case CallTileSetStride: {
    expectArgs(3);
    requireValue(0, TileVT);
    uint64_t StrideDim = constant(1);
    if (StrideDim > 1)
      failXAIcore(Name, "only dim 0 (row_stride) and 1 (col_stride) exist");
    emit(StrideDim == 0 ? RISCV::XAIcoreCfgSetxTMRowStride
                        : RISCV::XAIcoreCfgSetxTMColStride,
         {Args[0], scalar(2)});
    break;
  }
  case CallTileSetDType: {
    expectArgs(2);
    requireValue(0, TileVT);
    emit(RISCV::XAIcoreCfgSetxTCDtype, {Args[0], scalar(1)});
    break;
  }
  case CallTileSetFlags: {
    // tm.flags[1:0]：0 关闭 / 1 按行自增（rows*row_stride）/ 2 按列自增
    expectArgs(2);
    requireValue(0, TileVT);
    emit(RISCV::XAIcoreCfgSetxTMFlags, {Args[0], scalar(1)});
    break;
  }
  case CallTileSetValidZonePair: {
    expectArgs(3);
    requireValue(0, TileVT);
    emit(RISCV::XAIcoreCfgSetxTCRows, {Args[0], scalar(1)});
    emit(RISCV::XAIcoreCfgSetxTCCols, {Args[0], scalar(2)});
    break;
  }
  case CallTileSetValidZone: {
    expectArgs(3);
    requireValue(0, TileVT);
    uint64_t ZoneDim = constant(1);
    if (ZoneDim > 1)
      failXAIcore(Name, "only dim 0 (rows) and 1 (cols) exist");
    emit(ZoneDim == 0 ? RISCV::XAIcoreCfgSetxTCRows
                      : RISCV::XAIcoreCfgSetxTCCols,
         {Args[0], scalar(2)});
    break;
  }
  case CallTileSetTranspose: {
    expectArgs(2);
    requireValue(0, TileVT);
    emit(RISCV::XAIcoreCfgSetxTMTransform, {Args[0], scalar(1)});
    break;
  }
  case CallTileLoad: {
    expectArgs(1);
    requireValue(0, TileVT);
    emit(RISCV::XAIcoreTLoad, {Args[0]});
    break;
  }
  case CallTileGather: {
    // tile_gather_from_vec(tile, index)：表地址来自 tile 的 tm 描述符。
    expectArgs(2);
    requireValue(0, TileVT);
    requireValue(1, Vec32VT);
    emit(RISCV::XAIcoreTGather, {Args[0], Args[1]});
    break;
  }
  case CallVecGather: {
    // vec32_gather_from_vec(vec, index)：lane gather，表地址来自 vec 的 vm。
    expectArgs(2);
    requireValue(0, Vec32VT);
    requireValue(1, Vec32VT);
    emit(RISCV::XAIcoreVGather, {Args[0], Args[1]});
    break;
  }
  case CallTileWiden: {
    // tile_widen(tile, acc)：原地 widen，acc 的 dtype/zone 由用户先配置。
    expectArgs(2);
    requireValue(0, TileVT);
    requireValue(1, AccVT);
    emit(RISCV::XAIcoreTWiden, {Args[1], Args[0]});
    break;
  }
  case CallVecCopyDef: {
    // vec32_copy(vec) -> 新 Vec32 值（vcopy 的 def 形态）。
    // 目标寄存器是新值，必须先克隆 vc 配置（dtype/len），否则模拟器对未配置
    // 寄存器的 vcopy 会报 invalid_configuration。
    expectArgs(1);
    requireValue(0, Vec32VT);
    expectResult(Vec32VT);
    SDValue Dst = defineValue(RISCV::XAIcoreVec32Create, Vec32VT);
    emit(RISCV::XAIcoreCfgCopyVC, {Dst, Args[0]});
    SDValue CopyDst = emitDef(RISCV::XAIcoreVCopy, Vec32VT, {Args[0]});
    InVals.push_back(CopyDst);
    return Chain;
  }
  case CallTileStore: {
    expectArgs(1);
    requireValue(0, TileVT);
    emit(RISCV::XAIcoreTStore, {Args[0]});
    break;
  }
  case CallTileGetValidZone: {
    expectArgs(2);
    requireValue(0, TileVT);
    uint64_t GetDim = constant(1);
    if (GetDim > 1)
      failXAIcore(Name, "only dim 0 (rows) and 1 (cols) exist");
    expectXLenResult();
    unsigned Opcode = GetDim == 0 ? RISCV::XAIcoreCfgGetTCRows
                                  : RISCV::XAIcoreCfgGetTCCols;
    SDNode *Node =
        DAG.getMachineNode(Opcode, DL, DAG.getVTList(XLenVT, MVT::Other),
                           {Args[0], Chain});
    InVals.push_back(SDValue(Node, 0));
    return SDValue(Node, 1);
  }

  //===--------------------------------------------------------------------===//
  // Acc
  //===--------------------------------------------------------------------===//
  case CallAccSetData: {
    expectArgs(2);
    requireValue(0, AccVT);
    emit(RISCV::XAIcoreCfgSetxAMBaseAddr, {Args[0], scalar(1)});
    break;
  }
  case CallAccSetStrides: {
    expectArgs(3);
    requireValue(0, AccVT);
    emit(RISCV::XAIcoreCfgSetxAMRowStride, {Args[0], scalar(1)});
    emit(RISCV::XAIcoreCfgSetxAMColStride, {Args[0], scalar(2)});
    break;
  }
  case CallAccSetStride: {
    expectArgs(3);
    requireValue(0, AccVT);
    uint64_t StrideDim = constant(1);
    if (StrideDim > 1)
      failXAIcore(Name, "only dim 0 (row_stride) and 1 (col_stride) exist");
    emit(StrideDim == 0 ? RISCV::XAIcoreCfgSetxAMRowStride
                        : RISCV::XAIcoreCfgSetxAMColStride,
         {Args[0], scalar(2)});
    break;
  }
  case CallAccSetFlags: {
    // am.flags[1:0]：同 tm（访存后按行/列自增，只在成功后提交）
    expectArgs(2);
    requireValue(0, AccVT);
    emit(RISCV::XAIcoreCfgSetxAMFlags, {Args[0], scalar(1)});
    break;
  }
  case CallAccSetDType: {
    expectArgs(2);
    requireValue(0, AccVT);
    emit(RISCV::XAIcoreCfgSetxACDtype, {Args[0], scalar(1)});
    break;
  }
  case CallAccSetValidZonePair: {
    expectArgs(3);
    requireValue(0, AccVT);
    emit(RISCV::XAIcoreCfgSetxACRows, {Args[0], scalar(1)});
    emit(RISCV::XAIcoreCfgSetxACCols, {Args[0], scalar(2)});
    break;
  }
  case CallAccSetValidZone: {
    expectArgs(3);
    requireValue(0, AccVT);
    uint64_t ZoneDim = constant(1);
    if (ZoneDim > 1)
      failXAIcore(Name, "only dim 0 (rows) and 1 (cols) exist");
    emit(ZoneDim == 0 ? RISCV::XAIcoreCfgSetxACRows
                      : RISCV::XAIcoreCfgSetxACCols,
         {Args[0], scalar(2)});
    break;
  }
  case CallAccLoad: {
    expectArgs(1);
    requireValue(0, AccVT);
    emit(RISCV::XAIcoreALoad, {Args[0]});
    break;
  }
  case CallAccStore: {
    expectArgs(1);
    requireValue(0, AccVT);
    emit(RISCV::XAIcoreAStore, {Args[0]});
    break;
  }

  //===--------------------------------------------------------------------===//
  // Acc 逐元素二元运算（原地）：aadd/asub/amul/adiv <acc>, <acc>, <b>
  //===--------------------------------------------------------------------===//
  case CallAccAdd:
  case CallAccSub:
  case CallAccMul:
  case CallAccDiv: {
    expectArgs(2);
    requireValue(0, AccVT);
    requireValue(1, AccVT);
    unsigned Opcode = Call == CallAccAdd   ? RISCV::XAIcoreAAdd
                      : Call == CallAccSub ? RISCV::XAIcoreASub
                      : Call == CallAccMul ? RISCV::XAIcoreAMul
                                           : RISCV::XAIcoreADiv;
    // dest 和 src1 都是同一个 Acc（原地运算）。
    emit(Opcode, {Args[0], Args[0], Args[1]});
    break;
  }

  //===--------------------------------------------------------------------===//
  // Vec32：fill / 标量运算 / rsqrt / 配置 / load
  //===--------------------------------------------------------------------===//
  case CallVecFill:
  case CallVecAddScalar:
  case CallVecMulScalar: {
    expectArgs(2);
    requireValue(0, Vec32VT);
    if (Call == CallVecFill) {
      emit(RISCV::XAIcoreVFillX, {Args[0], fp32Operand(1)});
    } else {
      unsigned Opcode = Call == CallVecAddScalar ? RISCV::XAIcoreVAddX
                                                 : RISCV::XAIcoreVMulX;
      emit(Opcode, {Args[0], Args[0], fp32Operand(1)});
    }
    break;
  }
  case CallVecRsqrt: {
    expectArgs(1);
    requireValue(0, Vec32VT);
    emit(RISCV::XAIcoreVRsqrt, {Args[0], Args[0]});
    break;
  }
  case CallVecSetDType: {
    expectArgs(2);
    requireValue(0, Vec32VT);
    emit(RISCV::XAIcoreCfgSetxVCDtype, {Args[0], scalar(1)});
    break;
  }
  case CallVecSetFlags: {
    // vm.flags[0]：1 = 每次访存后按 len*stride 自增
    expectArgs(2);
    requireValue(0, Vec32VT);
    emit(RISCV::XAIcoreCfgSetxVMFlags, {Args[0], scalar(1)});
    break;
  }
  case CallVec8SetFlags: {
    // bm.flags[0]：vec8 的访存自增（目前内核没用 Vec8）
    expectArgs(2);
    requireValue(0, Vec8VT);
    emit(RISCV::XAIcoreCfgSetxBMFlags, {Args[0], scalar(1)});
    break;
  }
  case CallVecSetValidZone: {
    expectArgs(2);
    requireValue(0, Vec32VT);
    emit(RISCV::XAIcoreCfgSetxVCLen, {Args[0], scalar(1)});
    break;
  }
  case CallVecSetStride: {
    expectArgs(2);
    requireValue(0, Vec32VT);
    emit(RISCV::XAIcoreCfgSetxVMStride, {Args[0], scalar(1)});
    break;
  }
  case CallVecSetData: {
    expectArgs(2);
    requireValue(0, Vec32VT);
    emit(RISCV::XAIcoreCfgSetxVMBaseAddr, {Args[0], scalar(1)});
    break;
  }
  case CallVecLoad: {
    expectArgs(1);
    requireValue(0, Vec32VT);
    emit(RISCV::XAIcoreVLoad, {Args[0]});
    break;
  }
  case CallVecStore: {
    expectArgs(1);
    requireValue(0, Vec32VT);
    emit(RISCV::XAIcoreVStore, {Args[0]});
    break;
  }

  //===--------------------------------------------------------------------===//
  // 归约 / 广播：areduce.rows.sumsq v, a；amulv.byrow/bycol a, a, v
  //===--------------------------------------------------------------------===//
  case CallAccRowSumSq:
  case CallAccRowSum:
  case CallAccRowMax:
  case CallAccRowAbsMax: {
    // acc_row_{sum_sq,sum,max}_merge(acc, vec)：vec[r] = vec[r] Op 行归约
    expectArgs(2);
    requireValue(0, AccVT);
    requireValue(1, Vec32VT);
    unsigned ReduceOpc = Call == CallAccRowSumSq ? RISCV::XAIcoreAccRowSumSq
                         : Call == CallAccRowSum ? RISCV::XAIcoreAccRowSum
                         : Call == CallAccRowMax ? RISCV::XAIcoreAccRowMax
                                                 : RISCV::XAIcoreAccRowAbsMax;
    unsigned MergeOpc = (Call == CallAccRowMax || Call == CallAccRowAbsMax)
                            ? RISCV::XAIcoreVMax
                            : RISCV::XAIcoreVAdd;
    emitReduceMerge(ReduceOpc, MergeOpc);
    break;
  }
  case CallAccMulVec:
  case CallAccAddVec:
  case CallAccSubVec:
  case CallAccDivVec: {
    // acc_{mul,add,sub,div}_vec(acc, vec, col)：col=true → byrow，false → bycol。
    expectArgs(3);
    requireValue(0, AccVT);
    requireValue(1, Vec32VT);
    uint64_t Col = constant(2);
    unsigned RowOpc = Call == CallAccMulVec   ? RISCV::XAIcoreAccMulVecByRow
                      : Call == CallAccAddVec ? RISCV::XAIcoreAccAddVecByRow
                      : Call == CallAccSubVec ? RISCV::XAIcoreAccSubVecByRow
                                              : RISCV::XAIcoreAccDivVecByRow;
    unsigned ColOpc = Call == CallAccMulVec   ? RISCV::XAIcoreAccMulVecByCol
                      : Call == CallAccAddVec ? RISCV::XAIcoreAccAddVecByCol
                      : Call == CallAccSubVec ? RISCV::XAIcoreAccSubVecByCol
                                              : RISCV::XAIcoreAccDivVecByCol;
    emit(Col ? RowOpc : ColOpc, {Args[0], Args[0], Args[1]});
    break;
  }

  //===--------------------------------------------------------------------===//
  // Acc 标量 / fill / 一元
  //===--------------------------------------------------------------------===//
  case CallAccAddScalar:
  case CallAccMulScalar: {
    expectArgs(2);
    requireValue(0, AccVT);
    unsigned Opcode = Call == CallAccAddScalar ? RISCV::XAIcoreAAddX
                                               : RISCV::XAIcoreAMulX;
    emit(Opcode, {Args[0], Args[0], fp32Operand(1)});
    break;
  }
  case CallAccFill: {
    expectArgs(2);
    requireValue(0, AccVT);
    emit(RISCV::XAIcoreAFillX, {Args[0], fp32Operand(1)});
    break;
  }
  case CallAccExp: {
    // acc.exp() 是 e^x；ISA 只有 exp2，所以先乘 log2(e) 再 exp2。
    expectArgs(1);
    requireValue(0, AccVT);
    SDValue Log2e = DAG.getConstant(0x3FB8AA3B, DL, XLenVT);
    emit(RISCV::XAIcoreAMulX, {Args[0], Args[0], Log2e});
    emit(RISCV::XAIcoreAExp2, {Args[0], Args[0]});
    break;
  }
  case CallAccSilu: {
    expectArgs(1);
    requireValue(0, AccVT);
    emit(RISCV::XAIcoreASilu, {Args[0], Args[0]});
    break;
  }
  case CallAccAbs: {
    expectArgs(1);
    requireValue(0, AccVT);
    emit(RISCV::XAIcoreAAbs, {Args[0], Args[0]});
    break;
  }
  case CallAccToFp32: {
    expectArgs(1);
    requireValue(0, AccVT);
    emit(RISCV::XAIcoreACvt, {Args[0], Args[0]});
    break;
  }
  case CallAccRowArgMaxDef: {
    // argmax 的结果是 i32 向量（vc.dtype = 2）。
    expectArgs(1);
    requireValue(0, AccVT);
    expectResult(Vec32VT);
    InVals.push_back(emitReduceDef(RISCV::XAIcoreAccRowArgMax, 2));
    return Chain;
  }
  case CallAccRowSumDef:
  case CallAccRowMaxDef:
  case CallAccRowSumSqDef:
  case CallAccRowAbsMaxDef: {
    expectArgs(1);
    requireValue(0, AccVT);
    expectResult(Vec32VT);
    unsigned Opcode = Call == CallAccRowSumDef   ? RISCV::XAIcoreAccRowSum
                      : Call == CallAccRowMaxDef ? RISCV::XAIcoreAccRowMax
                      : Call == CallAccRowSumSqDef
                          ? RISCV::XAIcoreAccRowSumSq
                          : RISCV::XAIcoreAccRowAbsMax;
    InVals.push_back(emitReduceDef(Opcode));
    return Chain;
  }
  case CallAccMask: {
    // acc_mask(acc, diag, v_mask)：amaskx.tril aD, aS, xD, fill5。
    // v_mask 必须是 fill5 码表里的常量；默认的 -FLT_MAX 按 -inf 处理
    // （softmax 里 -FLT_MAX 与 -inf 的 exp 结果同为 0）。
    expectArgs(3);
    requireValue(0, AccVT);
    // 软浮点 ABI 下 float 实参可能已经变成整数的位模式，两种都接受。
    float Value;
    if (auto *FP = dyn_cast<ConstantFPSDNode>(Args[2])) {
      Value = FP->getValueAPF().convertToFloat();
    } else if (auto *C = dyn_cast<ConstantSDNode>(Args[2])) {
      const uint32_t Bits = static_cast<uint32_t>(C->getZExtValue());
      std::memcpy(&Value, &Bits, sizeof(Value));
    } else {
      failXAIcore(Name, "mask fill value must be a compile-time constant");
    }
    int Code = -1;
    if (Value <= -3.0e38f)
      Code = 8; // -inf
    else if (Value == 0.0f) Code = 0;
    else if (Value == 1.0f) Code = 1;
    else if (Value == -1.0f) Code = 2;
    else if (Value == 2.0f) Code = 3;
    else if (Value == -2.0f) Code = 4;
    else if (Value == 0.5f) Code = 5;
    else if (Value == -0.5f) Code = 6;
    else if (Value == std::numeric_limits<float>::infinity()) Code = 7;
    if (Code < 0)
      failXAIcore(Name, "unsupported mask fill value");
    // fill5 是“立即数操作数”：必须用 TargetConstant，否则选择器会把它
    // 物化成一条 ADDI 并当成寄存器操作数。
    emit(RISCV::XAIcoreAMaskTril,
         {Args[0], Args[0], scalar(1),
          DAG.getTargetConstant(Code, DL, XLenVT)});
    break;
  }
  case CallAccQuantExt: {
    // acc_quant_ext(acc, scale, tile)：tquantx 用外部 scale，不改 scale。
    expectArgs(3);
    requireValue(0, AccVT);
    requireValue(1, Vec32VT);
    requireValue(2, TileVT);
    emit(RISCV::XAIcoreTQuantX, {Args[2], Args[1], Args[0]});
    break;
  }
  case CallAccQuant: {
    // acc_quant(acc, vec, tile)：tquant 自己算每行 scale 并写回 vec。
    // 指令操作数顺序是 (tile, vscale, acc)。
    expectArgs(3);
    requireValue(0, AccVT);
    requireValue(1, Vec32VT);
    requireValue(2, TileVT);
    emit(RISCV::XAIcoreTQuant, {Args[2], Args[1], Args[0]});
    break;
  }

  //===--------------------------------------------------------------------===//
  // vec32 二元 / exp
  //===--------------------------------------------------------------------===//
  case CallVecAdd:
  case CallVecSub:
  case CallVecMul:
  case CallVecDiv:
  case CallVecMax: {
    expectArgs(2);
    requireValue(0, Vec32VT);
    requireValue(1, Vec32VT);
    unsigned Opcode = Call == CallVecAdd   ? RISCV::XAIcoreVAdd
                      : Call == CallVecSub ? RISCV::XAIcoreVSub
                      : Call == CallVecMul ? RISCV::XAIcoreVMul
                      : Call == CallVecDiv ? RISCV::XAIcoreVDiv
                                           : RISCV::XAIcoreVMax;
    emit(Opcode, {Args[0], Args[0], Args[1]});
    break;
  }
  case CallVecExp: {
    // vec32.exp() 同样是 e^x：先乘 log2(e) 再 exp2。
    expectArgs(1);
    requireValue(0, Vec32VT);
    SDValue Log2e = DAG.getConstant(0x3FB8AA3B, DL, XLenVT);
    emit(RISCV::XAIcoreVMulX, {Args[0], Args[0], Log2e});
    emit(RISCV::XAIcoreVExp2, {Args[0], Args[0]});
    break;
  }

  //===--------------------------------------------------------------------===//
  // 矩阵乘：mma.nn.{zero,acc}.i8.i32 <acc>, <tileA>, <tileB>
  //===--------------------------------------------------------------------===//
  case CallMatmul: {
    // tile_matmul(tileA, tileB, acc, zero, lt, rt)
    //   lt=false, rt=false -> mma.nn.{zero,acc}
    //   lt=false, rt=true  -> mma.nt.{zero,acc}（右侧按 N×K 存放）
    //   lt=true            -> ISA 没有左侧转置，报错
    expectArgs(6);
    requireValue(0, TileVT);
    requireValue(1, TileVT);
    requireValue(2, AccVT);
    uint64_t Zero = constant(3);
    uint64_t LeftTranspose = constant(4);
    uint64_t RightTranspose = constant(5);
    if (LeftTranspose)
      failXAIcore(Name,
                  "only nn/nt mma variants are supported (left operand must "
                  "not be transposed)");
    unsigned Opcode;
    if (RightTranspose)
      Opcode = Zero ? RISCV::XAIcoreMMANTZero : RISCV::XAIcoreMMANTAcc;
    else
      Opcode = Zero ? RISCV::XAIcoreMMANNZero : RISCV::XAIcoreMMANNAcc;
    emit(Opcode, {Args[2], Args[0], Args[1]});
    break;
  }

  case CallUnsupported:
    llvm_unreachable("unsupported YAI call must be diagnosed earlier");
  }
  return Chain;
}
