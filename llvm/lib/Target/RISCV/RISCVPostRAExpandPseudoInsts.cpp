//===-- RISCVPostRAExpandPseudoInsts.cpp - Expand pseudo instrs ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a pass that expands the pseudo instruction pseudolisimm32
// into target instructions. This pass should be run during the post-regalloc
// passes, before post RA scheduling.
//
//===----------------------------------------------------------------------===//

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetOpcodes.h"

using namespace llvm;

#define RISCV_POST_RA_EXPAND_PSEUDO_NAME                                       \
  "RISC-V post-regalloc pseudo instruction expansion pass"

namespace {

class RISCVPostRAExpandPseudo : public MachineFunctionPass {
public:
  const RISCVInstrInfo *TII;
  static char ID;

  RISCVPostRAExpandPseudo() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override {
    return RISCV_POST_RA_EXPAND_PSEUDO_NAME;
  }

private:
  bool expandMBB(MachineBasicBlock &MBB);
  bool expandMI(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                MachineBasicBlock::iterator &NextMBBI);
  bool expandMovImm(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI);
  bool expandMovAddr(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI);
};

char RISCVPostRAExpandPseudo::ID = 0;

bool RISCVPostRAExpandPseudo::runOnMachineFunction(MachineFunction &MF) {
  TII = static_cast<const RISCVInstrInfo *>(MF.getSubtarget().getInstrInfo());
  bool Modified = false;
  for (auto &MBB : MF)
    Modified |= expandMBB(MBB);
  return Modified;
}

bool RISCVPostRAExpandPseudo::expandMBB(MachineBasicBlock &MBB) {
  bool Modified = false;

  MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
  while (MBBI != E) {
    MachineBasicBlock::iterator NMBBI = std::next(MBBI);
    Modified |= expandMI(MBB, MBBI, NMBBI);
    MBBI = NMBBI;
  }

  return Modified;
}

bool RISCVPostRAExpandPseudo::expandMI(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator MBBI,
                                       MachineBasicBlock::iterator &NextMBBI) {
  switch (MBBI->getOpcode()) {
  case RISCV::PseudoMovImm:
    return expandMovImm(MBB, MBBI);
  case RISCV::PseudoMovAddr:
    return expandMovAddr(MBB, MBBI);
  // acc/vec 运算的原地/值形式伪指令：统一展开。
  // 原地：D 取 S 的物理寄存器；值形式：先把描述符（ac 或 vc）克隆到 D。
  case RISCV::XAIcoreAAddXInPlace:
  case RISCV::XAIcoreAAddXVal:
  case RISCV::XAIcoreAMulXInPlace:
  case RISCV::XAIcoreAMulXVal:
  case RISCV::XAIcoreAAddInPlace:
  case RISCV::XAIcoreAAddVal:
  case RISCV::XAIcoreASubInPlace:
  case RISCV::XAIcoreASubVal:
  case RISCV::XAIcoreAMulInPlace:
  case RISCV::XAIcoreAMulVal:
  case RISCV::XAIcoreADivInPlace:
  case RISCV::XAIcoreADivVal:
  case RISCV::XAIcoreAccAddVecByRowInPlace:
  case RISCV::XAIcoreAccAddVecByRowVal:
  case RISCV::XAIcoreAccAddVecByColInPlace:
  case RISCV::XAIcoreAccAddVecByColVal:
  case RISCV::XAIcoreAccSubVecByRowInPlace:
  case RISCV::XAIcoreAccSubVecByRowVal:
  case RISCV::XAIcoreAccSubVecByColInPlace:
  case RISCV::XAIcoreAccSubVecByColVal:
  case RISCV::XAIcoreAccMulVecByRowInPlace:
  case RISCV::XAIcoreAccMulVecByRowVal:
  case RISCV::XAIcoreAccMulVecByColInPlace:
  case RISCV::XAIcoreAccMulVecByColVal:
  case RISCV::XAIcoreAccDivVecByRowInPlace:
  case RISCV::XAIcoreAccDivVecByRowVal:
  case RISCV::XAIcoreAccDivVecByColInPlace:
  case RISCV::XAIcoreAccDivVecByColVal:
  case RISCV::XAIcoreVAddInPlace:
  case RISCV::XAIcoreVAddVal:
  case RISCV::XAIcoreVSubInPlace:
  case RISCV::XAIcoreVSubVal:
  case RISCV::XAIcoreVMulInPlace:
  case RISCV::XAIcoreVMulVal:
  case RISCV::XAIcoreVDivInPlace:
  case RISCV::XAIcoreVDivVal:
  case RISCV::XAIcoreVMaxInPlace:
  case RISCV::XAIcoreVMaxVal:
  case RISCV::XAIcoreVAddXInPlace:
  case RISCV::XAIcoreVAddXVal:
  case RISCV::XAIcoreVMulXInPlace:
  case RISCV::XAIcoreVMulXVal:
  case RISCV::XAIcoreAAbsInPlace:
  case RISCV::XAIcoreAAbsVal:
  case RISCV::XAIcoreARcpInPlace:
  case RISCV::XAIcoreARcpVal:
  case RISCV::XAIcoreAExp2InPlace:
  case RISCV::XAIcoreAExp2Val:
  case RISCV::XAIcoreASiluInPlace:
  case RISCV::XAIcoreASiluVal:
  case RISCV::XAIcoreVRcpInPlace:
  case RISCV::XAIcoreVRcpVal:
  case RISCV::XAIcoreVRsqrtInPlace:
  case RISCV::XAIcoreVRsqrtVal:
  case RISCV::XAIcoreVExp2InPlace:
  case RISCV::XAIcoreVExp2Val: {
    struct Entry { unsigned InPlace, Val, Real; bool IsVec; };
    static const Entry Table[] = {
      {RISCV::XAIcoreAAddXInPlace, RISCV::XAIcoreAAddXVal, RISCV::XAIcoreAAddX, false},
      {RISCV::XAIcoreAMulXInPlace, RISCV::XAIcoreAMulXVal, RISCV::XAIcoreAMulX, false},
      {RISCV::XAIcoreAAddInPlace, RISCV::XAIcoreAAddVal, RISCV::XAIcoreAAdd, false},
      {RISCV::XAIcoreASubInPlace, RISCV::XAIcoreASubVal, RISCV::XAIcoreASub, false},
      {RISCV::XAIcoreAMulInPlace, RISCV::XAIcoreAMulVal, RISCV::XAIcoreAMul, false},
      {RISCV::XAIcoreADivInPlace, RISCV::XAIcoreADivVal, RISCV::XAIcoreADiv, false},
      {RISCV::XAIcoreAccAddVecByRowInPlace, RISCV::XAIcoreAccAddVecByRowVal, RISCV::XAIcoreAccAddVecByRow, false},
      {RISCV::XAIcoreAccAddVecByColInPlace, RISCV::XAIcoreAccAddVecByColVal, RISCV::XAIcoreAccAddVecByCol, false},
      {RISCV::XAIcoreAccSubVecByRowInPlace, RISCV::XAIcoreAccSubVecByRowVal, RISCV::XAIcoreAccSubVecByRow, false},
      {RISCV::XAIcoreAccSubVecByColInPlace, RISCV::XAIcoreAccSubVecByColVal, RISCV::XAIcoreAccSubVecByCol, false},
      {RISCV::XAIcoreAccMulVecByRowInPlace, RISCV::XAIcoreAccMulVecByRowVal, RISCV::XAIcoreAccMulVecByRow, false},
      {RISCV::XAIcoreAccMulVecByColInPlace, RISCV::XAIcoreAccMulVecByColVal, RISCV::XAIcoreAccMulVecByCol, false},
      {RISCV::XAIcoreAccDivVecByRowInPlace, RISCV::XAIcoreAccDivVecByRowVal, RISCV::XAIcoreAccDivVecByRow, false},
      {RISCV::XAIcoreAccDivVecByColInPlace, RISCV::XAIcoreAccDivVecByColVal, RISCV::XAIcoreAccDivVecByCol, false},
      {RISCV::XAIcoreVAddInPlace, RISCV::XAIcoreVAddVal, RISCV::XAIcoreVAdd, true},
      {RISCV::XAIcoreVSubInPlace, RISCV::XAIcoreVSubVal, RISCV::XAIcoreVSub, true},
      {RISCV::XAIcoreVMulInPlace, RISCV::XAIcoreVMulVal, RISCV::XAIcoreVMul, true},
      {RISCV::XAIcoreVDivInPlace, RISCV::XAIcoreVDivVal, RISCV::XAIcoreVDiv, true},
      {RISCV::XAIcoreVMaxInPlace, RISCV::XAIcoreVMaxVal, RISCV::XAIcoreVMax, true},
      {RISCV::XAIcoreVAddXInPlace, RISCV::XAIcoreVAddXVal, RISCV::XAIcoreVAddX, true},
      {RISCV::XAIcoreVMulXInPlace, RISCV::XAIcoreVMulXVal, RISCV::XAIcoreVMulX, true},
      {RISCV::XAIcoreAAbsInPlace, RISCV::XAIcoreAAbsVal, RISCV::XAIcoreAAbs, false},
      {RISCV::XAIcoreAExp2InPlace, RISCV::XAIcoreAExp2Val, RISCV::XAIcoreAExp2, false},
      {RISCV::XAIcoreASiluInPlace, RISCV::XAIcoreASiluVal, RISCV::XAIcoreASilu, false},
      {RISCV::XAIcoreVExp2InPlace, RISCV::XAIcoreVExp2Val, RISCV::XAIcoreVExp2, true},
      {RISCV::XAIcoreARcpInPlace, RISCV::XAIcoreARcpVal, RISCV::XAIcoreARcp, false},
      {RISCV::XAIcoreVRcpInPlace, RISCV::XAIcoreVRcpVal, RISCV::XAIcoreVRcp, true},
      {RISCV::XAIcoreVRsqrtInPlace, RISCV::XAIcoreVRsqrtVal, RISCV::XAIcoreVRsqrt, true},
    };
    const unsigned Opc = MBBI->getOpcode();
    const Entry *E = nullptr;
    for (const Entry &Cand : Table)
      if (Cand.InPlace == Opc || Cand.Val == Opc) { E = &Cand; break; }
    if (!E)
      llvm_unreachable("unknown acc/vec arithmetic pseudo");
    const bool IsValue = (Opc == E->Val);
    Register Src = MBBI->getOperand(IsValue ? 1 : 0).getReg();
    Register Dst = IsValue ? MBBI->getOperand(0).getReg() : Src;
    if (IsValue) {
      // 新寄存器的描述符是空的，必须先克隆源寄存器的配置。
      unsigned CfgCopy = E->IsVec ? RISCV::XAIcoreCfgCopyVC : RISCV::XAIcoreCfgCopyAC;
      BuildMI(MBB, MBBI, MBBI->getDebugLoc(), TII->get(CfgCopy))
          .addReg(Dst)
          .addReg(Src);
    }
    // 真实指令：D 后面跟原来的 ins 操作数（跳过 D 本身）。
    auto NewMI = BuildMI(MBB, MBBI, MBBI->getDebugLoc(), TII->get(E->Real), Dst)
                     .addReg(Src);   // 真实指令的 $s1（之前漏了，导致操作数少一个）
    for (unsigned i = (IsValue ? 2 : 1), e = MBBI->getNumOperands(); i != e; ++i)
      NewMI.add(MBBI->getOperand(i));
    MBBI->eraseFromParent();
    return true;
  }
  case RISCV::XAIcoreTCreate:
  case RISCV::XAIcoreACreate:
  case RISCV::XAIcoreVec32Create:
  case RISCV::XAIcoreVec8Create: {
    // *_create 只是为一个值分配物理寄存器，没有真实的机器动作。
    // 用 IMPLICIT_DEF 只给出“这个物理寄存器被定义”的信息：它不产生任何
    // 机器码（AsmPrinter 会跳过），因此也不会像 tcopy/fillx 那样要求寄存
    // 器已经配置好（Tile-NPU-Sim 对未配置寄存器的 copy 会报
    // invalid_configuration）。
    Register DstReg = MBBI->getOperand(0).getReg();
    BuildMI(MBB, MBBI, MBBI->getDebugLoc(),
            TII->get(TargetOpcode::IMPLICIT_DEF), DstReg);
    MBBI->eraseFromParent();
    return true;
  }
  default:
    return false;
  }
}

bool RISCVPostRAExpandPseudo::expandMovImm(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator MBBI) {
  DebugLoc DL = MBBI->getDebugLoc();

  int64_t Val = MBBI->getOperand(1).getImm();

  Register DstReg = MBBI->getOperand(0).getReg();
  bool DstIsDead = MBBI->getOperand(0).isDead();
  bool Renamable = MBBI->getOperand(0).isRenamable();

  TII->movImm(MBB, MBBI, DL, DstReg, Val, MachineInstr::NoFlags, Renamable,
              DstIsDead);

  MBBI->eraseFromParent();
  return true;
}

bool RISCVPostRAExpandPseudo::expandMovAddr(MachineBasicBlock &MBB,
                                            MachineBasicBlock::iterator MBBI) {
  DebugLoc DL = MBBI->getDebugLoc();

  Register DstReg = MBBI->getOperand(0).getReg();
  bool DstIsDead = MBBI->getOperand(0).isDead();
  bool Renamable = MBBI->getOperand(0).isRenamable();

  BuildMI(MBB, MBBI, DL, TII->get(RISCV::LUI))
      .addReg(DstReg, RegState::Define | getRenamableRegState(Renamable))
      .add(MBBI->getOperand(1));
  BuildMI(MBB, MBBI, DL, TII->get(RISCV::ADDI))
      .addReg(DstReg, RegState::Define | getDeadRegState(DstIsDead) |
                          getRenamableRegState(Renamable))
      .addReg(DstReg, RegState::Kill | getRenamableRegState(Renamable))
      .add(MBBI->getOperand(2));
  MBBI->eraseFromParent();
  return true;
}

} // end of anonymous namespace

INITIALIZE_PASS(RISCVPostRAExpandPseudo, "riscv-post-ra-expand-pseudo",
                RISCV_POST_RA_EXPAND_PSEUDO_NAME, false, false)
namespace llvm {

FunctionPass *createRISCVPostRAExpandPseudoPass() {
  return new RISCVPostRAExpandPseudo();
}

} // end of namespace llvm
