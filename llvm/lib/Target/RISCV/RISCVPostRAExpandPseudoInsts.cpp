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
