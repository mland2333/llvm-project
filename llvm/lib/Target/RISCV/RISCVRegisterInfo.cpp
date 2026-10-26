//===-- RISCVRegisterInfo.cpp - RISC-V Register Information -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the RISC-V implementation of the TargetRegisterInfo class.
//
//===----------------------------------------------------------------------===//

#include "RISCVRegisterInfo.h"
#include "RISCV.h"
#include "RISCVSubtarget.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/ErrorHandling.h"
#include <map>

#define GET_REGINFO_TARGET_DESC
#include "RISCVGenRegisterInfo.inc"

using namespace llvm;

static cl::opt<bool> DisableCostPerUse("riscv-disable-cost-per-use",
                                       cl::init(false), cl::Hidden);
static cl::opt<bool>
    DisableRegAllocHints("riscv-disable-regalloc-hints", cl::Hidden,
                         cl::init(false),
                         cl::desc("Disable two address hints for register "
                                  "allocation"));

static_assert(RISCV::X1 == RISCV::X0 + 1, "Register list not consecutive");
static_assert(RISCV::X31 == RISCV::X0 + 31, "Register list not consecutive");
static_assert(RISCV::F1_H == RISCV::F0_H + 1, "Register list not consecutive");
static_assert(RISCV::F31_H == RISCV::F0_H + 31,
              "Register list not consecutive");
static_assert(RISCV::F1_F == RISCV::F0_F + 1, "Register list not consecutive");
static_assert(RISCV::F31_F == RISCV::F0_F + 31,
              "Register list not consecutive");
static_assert(RISCV::F1_D == RISCV::F0_D + 1, "Register list not consecutive");
static_assert(RISCV::F31_D == RISCV::F0_D + 31,
              "Register list not consecutive");
static_assert(RISCV::F1_Q == RISCV::F0_Q + 1, "Register list not consecutive");
static_assert(RISCV::F31_Q == RISCV::F0_Q + 31,
              "Register list not consecutive");
static_assert(RISCV::V1 == RISCV::V0 + 1, "Register list not consecutive");
static_assert(RISCV::V31 == RISCV::V0 + 31, "Register list not consecutive");

RISCVRegisterInfo::RISCVRegisterInfo(unsigned HwMode)
    : RISCVGenRegisterInfo(RISCV::X1, /*DwarfFlavour*/0, /*EHFlavor*/0,
                           /*PC*/0, HwMode) {}

const MCPhysReg *
RISCVRegisterInfo::getIPRACSRegs(const MachineFunction *MF) const {
  return CSR_IPRA_SaveList;
}

const MCPhysReg *
RISCVRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  auto &Subtarget = MF->getSubtarget<RISCVSubtarget>();
  if (MF->getFunction().getCallingConv() == CallingConv::GHC)
    return CSR_NoRegs_SaveList;
  if (MF->getFunction().getCallingConv() == CallingConv::PreserveMost)
    return Subtarget.hasStdExtE() ? CSR_RT_MostRegs_RVE_SaveList
                                  : CSR_RT_MostRegs_SaveList;
  if (MF->getFunction().hasFnAttribute("interrupt")) {
    if (Subtarget.hasVInstructions()) {
      if (Subtarget.hasStdExtD())
        return Subtarget.hasStdExtE() ? CSR_XLEN_F64_V_Interrupt_RVE_SaveList
                                      : CSR_XLEN_F64_V_Interrupt_SaveList;
      if (Subtarget.hasStdExtF())
        return Subtarget.hasStdExtE() ? CSR_XLEN_F32_V_Interrupt_RVE_SaveList
                                      : CSR_XLEN_F32_V_Interrupt_SaveList;
      return Subtarget.hasStdExtE() ? CSR_XLEN_V_Interrupt_RVE_SaveList
                                    : CSR_XLEN_V_Interrupt_SaveList;
    }
    if (Subtarget.hasStdExtD())
      return Subtarget.hasStdExtE() ? CSR_XLEN_F64_Interrupt_RVE_SaveList
                                    : CSR_XLEN_F64_Interrupt_SaveList;
    if (Subtarget.hasStdExtF())
      return Subtarget.hasStdExtE() ? CSR_XLEN_F32_Interrupt_RVE_SaveList
                                    : CSR_XLEN_F32_Interrupt_SaveList;
    return Subtarget.hasStdExtE() ? CSR_Interrupt_RVE_SaveList
                                  : CSR_Interrupt_SaveList;
  }

  bool HasVectorCSR =
      MF->getFunction().getCallingConv() == CallingConv::RISCV_VectorCall &&
      Subtarget.hasVInstructions();

  switch (Subtarget.getTargetABI()) {
  default:
    llvm_unreachable("Unrecognized ABI");
  case RISCVABI::ABI_ILP32E:
  case RISCVABI::ABI_LP64E:
    return CSR_ILP32E_LP64E_SaveList;
  case RISCVABI::ABI_ILP32:
  case RISCVABI::ABI_LP64:
    if (HasVectorCSR)
      return CSR_ILP32_LP64_V_SaveList;
    return CSR_ILP32_LP64_SaveList;
  case RISCVABI::ABI_ILP32F:
  case RISCVABI::ABI_LP64F:
    if (HasVectorCSR)
      return CSR_ILP32F_LP64F_V_SaveList;
    return CSR_ILP32F_LP64F_SaveList;
  case RISCVABI::ABI_ILP32D:
  case RISCVABI::ABI_LP64D:
    if (HasVectorCSR)
      return CSR_ILP32D_LP64D_V_SaveList;
    return CSR_ILP32D_LP64D_SaveList;
  }
}

BitVector RISCVRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  const RISCVFrameLowering *TFI = getFrameLowering(MF);
  BitVector Reserved(getNumRegs());
  auto &Subtarget = MF.getSubtarget<RISCVSubtarget>();

  for (size_t Reg = 0; Reg < getNumRegs(); Reg++) {
    // Mark any GPRs requested to be reserved as such
    if (Subtarget.isRegisterReservedByUser(Reg))
      markSuperRegs(Reserved, Reg);

    // Mark all the registers defined as constant in TableGen as reserved.
    if (isConstantPhysReg(Reg))
      markSuperRegs(Reserved, Reg);
  }

  // Use markSuperRegs to ensure any register aliases are also reserved
  markSuperRegs(Reserved, RISCV::X2_H); // sp
  markSuperRegs(Reserved, RISCV::X3_H); // gp
  markSuperRegs(Reserved, RISCV::X4_H); // tp
  if (TFI->hasFP(MF))
    markSuperRegs(Reserved, RISCV::X8_H); // fp
  // Reserve the base register if we need to realign the stack and allocate
  // variable-sized objects at runtime.
  if (TFI->hasBP(MF))
    markSuperRegs(Reserved, RISCVABI::getBPReg()); // bp

  // Additionally reserve dummy register used to form the register pair
  // beginning with 'x0' for instructions that take register pairs.
  markSuperRegs(Reserved, RISCV::DUMMY_REG_PAIR_WITH_X0);

  // There are only 16 GPRs for RVE.
  if (Subtarget.hasStdExtE())
    for (MCPhysReg Reg = RISCV::X16_H; Reg <= RISCV::X31_H; Reg++)
      markSuperRegs(Reserved, Reg);

  // V registers for code generation. We handle them manually.
  markSuperRegs(Reserved, RISCV::VL);
  markSuperRegs(Reserved, RISCV::VTYPE);
  markSuperRegs(Reserved, RISCV::VXSAT);
  markSuperRegs(Reserved, RISCV::VXRM);

  // Floating point environment registers.
  markSuperRegs(Reserved, RISCV::FRM);
  markSuperRegs(Reserved, RISCV::FFLAGS);

  // SiFive VCIX state registers.
  markSuperRegs(Reserved, RISCV::SF_VCIX_STATE);

  if (MF.getFunction().getCallingConv() == CallingConv::GRAAL) {
    if (Subtarget.hasStdExtE())
      report_fatal_error("Graal reserved registers do not exist in RVE");
    markSuperRegs(Reserved, RISCV::X23_H);
    markSuperRegs(Reserved, RISCV::X27_H);
  }

  // Shadow stack pointer.
  markSuperRegs(Reserved, RISCV::SSP);

  assert(checkAllSuperRegsMarked(Reserved));
  return Reserved;
}

bool RISCVRegisterInfo::isAsmClobberable(const MachineFunction &MF,
                                         MCRegister PhysReg) const {
  return !MF.getSubtarget().isRegisterReservedByUser(PhysReg);
}

const uint32_t *RISCVRegisterInfo::getNoPreservedMask() const {
  return CSR_NoRegs_RegMask;
}

void RISCVRegisterInfo::adjustReg(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator II,
                                  const DebugLoc &DL, Register DestReg,
                                  Register SrcReg, StackOffset Offset,
                                  MachineInstr::MIFlag Flag,
                                  MaybeAlign RequiredAlign) const {

  if (DestReg == SrcReg && !Offset.getFixed() && !Offset.getScalable())
    return;

  MachineFunction &MF = *MBB.getParent();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const RISCVSubtarget &ST = MF.getSubtarget<RISCVSubtarget>();
  const RISCVInstrInfo *TII = ST.getInstrInfo();

  // Optimize compile time offset case
  if (Offset.getScalable()) {
    if (auto VLEN = ST.getRealVLen()) {
      // 1. Multiply the number of v-slots by the (constant) length of register
      const int64_t VLENB = *VLEN / 8;
      assert(Offset.getScalable() % RISCV::RVVBytesPerBlock == 0 &&
             "Reserve the stack by the multiple of one vector size.");
      const int64_t NumOfVReg = Offset.getScalable() / 8;
      const int64_t FixedOffset = NumOfVReg * VLENB;
      if (!isInt<32>(FixedOffset)) {
        report_fatal_error(
            "Frame size outside of the signed 32-bit range not supported");
      }
      Offset = StackOffset::getFixed(FixedOffset + Offset.getFixed());
    }
  }

  bool KillSrcReg = false;

  if (Offset.getScalable()) {
    unsigned ScalableAdjOpc = RISCV::ADD;
    int64_t ScalableValue = Offset.getScalable();
    if (ScalableValue < 0) {
      ScalableValue = -ScalableValue;
      ScalableAdjOpc = RISCV::SUB;
    }
    // Get vlenb and multiply vlen with the number of vector registers.
    Register ScratchReg = DestReg;
    if (DestReg == SrcReg)
      ScratchReg = MRI.createVirtualRegister(&RISCV::GPRRegClass);

    assert(ScalableValue > 0 && "There is no need to get VLEN scaled value.");
    assert(ScalableValue % RISCV::RVVBytesPerBlock == 0 &&
           "Reserve the stack by the multiple of one vector size.");
    assert(isInt<32>(ScalableValue / RISCV::RVVBytesPerBlock) &&
           "Expect the number of vector registers within 32-bits.");
    uint32_t NumOfVReg = ScalableValue / RISCV::RVVBytesPerBlock;
    // Only use vsetvli rather than vlenb if adjusting in the prologue or
    // epilogue, otherwise it may disturb the VTYPE and VL status.
    bool IsPrologueOrEpilogue =
        Flag == MachineInstr::FrameSetup || Flag == MachineInstr::FrameDestroy;
    bool UseVsetvliRatherThanVlenb =
        IsPrologueOrEpilogue && ST.preferVsetvliOverReadVLENB();
    if (UseVsetvliRatherThanVlenb && (NumOfVReg == 1 || NumOfVReg == 2 ||
                                      NumOfVReg == 4 || NumOfVReg == 8)) {
      BuildMI(MBB, II, DL, TII->get(RISCV::PseudoReadVLENBViaVSETVLIX0),
              ScratchReg)
          .addImm(NumOfVReg)
          .setMIFlag(Flag);
      BuildMI(MBB, II, DL, TII->get(ScalableAdjOpc), DestReg)
          .addReg(SrcReg)
          .addReg(ScratchReg, RegState::Kill)
          .setMIFlag(Flag);
    } else {
      if (UseVsetvliRatherThanVlenb)
        BuildMI(MBB, II, DL, TII->get(RISCV::PseudoReadVLENBViaVSETVLIX0),
                ScratchReg)
            .addImm(1)
            .setMIFlag(Flag);
      else
        BuildMI(MBB, II, DL, TII->get(RISCV::PseudoReadVLENB), ScratchReg)
            .setMIFlag(Flag);

      if (ScalableAdjOpc == RISCV::ADD && ST.hasStdExtZba() &&
          (NumOfVReg == 2 || NumOfVReg == 4 || NumOfVReg == 8)) {
        unsigned Opc = NumOfVReg == 2
                           ? RISCV::SH1ADD
                           : (NumOfVReg == 4 ? RISCV::SH2ADD : RISCV::SH3ADD);
        BuildMI(MBB, II, DL, TII->get(Opc), DestReg)
            .addReg(ScratchReg, RegState::Kill)
            .addReg(SrcReg)
            .setMIFlag(Flag);
      } else {
        TII->mulImm(MF, MBB, II, DL, ScratchReg, NumOfVReg, Flag);
        BuildMI(MBB, II, DL, TII->get(ScalableAdjOpc), DestReg)
            .addReg(SrcReg)
            .addReg(ScratchReg, RegState::Kill)
            .setMIFlag(Flag);
      }
    }
    SrcReg = DestReg;
    KillSrcReg = true;
  }

  int64_t Val = Offset.getFixed();
  if (DestReg == SrcReg && Val == 0)
    return;

  const uint64_t Align = RequiredAlign.valueOrOne().value();

  if (isInt<12>(Val)) {
    BuildMI(MBB, II, DL, TII->get(RISCV::ADDI), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrcReg))
        .addImm(Val)
        .setMIFlag(Flag);
    return;
  }

  // Use the QC_E_ADDI instruction from the Xqcilia extension that can take a
  // signed 26-bit immediate.
  if (ST.hasVendorXqcilia() && isInt<26>(Val)) {
    // The one case where using this instruction is sub-optimal is if Val can be
    // materialized with a single compressible LUI and following add/sub is also
    // compressible. Avoid doing this if that is the case.
    int Hi20 = (Val & 0xFFFFF000) >> 12;
    bool IsCompressLUI =
        ((Val & 0xFFF) == 0) && (Hi20 != 0) &&
        (isUInt<5>(Hi20) || (Hi20 >= 0xfffe0 && Hi20 <= 0xfffff));
    bool IsCompressAddSub =
        (SrcReg == DestReg) &&
        ((Val > 0 && RISCV::GPRNoX0RegClass.contains(SrcReg)) ||
         (Val < 0 && RISCV::GPRCRegClass.contains(SrcReg)));

    if (!(IsCompressLUI && IsCompressAddSub)) {
      BuildMI(MBB, II, DL, TII->get(RISCV::QC_E_ADDI), DestReg)
          .addReg(SrcReg, getKillRegState(KillSrcReg))
          .addImm(Val)
          .setMIFlag(Flag);
      return;
    }
  }

  // Try to split the offset across two ADDIs. We need to keep the intermediate
  // result aligned after each ADDI.  We need to determine the maximum value we
  // can put in each ADDI. In the negative direction, we can use -2048 which is
  // always sufficiently aligned. In the positive direction, we need to find the
  // largest 12-bit immediate that is aligned.  Exclude -4096 since it can be
  // created with LUI.
  assert(Align < 2048 && "Required alignment too large");
  int64_t MaxPosAdjStep = 2048 - Align;
  if (Val > -4096 && Val <= (2 * MaxPosAdjStep)) {
    int64_t FirstAdj = Val < 0 ? -2048 : MaxPosAdjStep;
    Val -= FirstAdj;
    BuildMI(MBB, II, DL, TII->get(RISCV::ADDI), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrcReg))
        .addImm(FirstAdj)
        .setMIFlag(Flag);
    BuildMI(MBB, II, DL, TII->get(RISCV::ADDI), DestReg)
        .addReg(DestReg, RegState::Kill)
        .addImm(Val)
        .setMIFlag(Flag);
    return;
  }

  // Use shNadd if doing so lets us materialize a 12 bit immediate with a single
  // instruction.  This saves 1 instruction over the full lui/addi+add fallback
  // path.  We avoid anything which can be done with a single lui as it might
  // be compressible.  Note that the sh1add case is fully covered by the 2x addi
  // case just above and is thus omitted.
  if (ST.hasStdExtZba() && (Val & 0xFFF) != 0) {
    unsigned Opc = 0;
    if (isShiftedInt<12, 3>(Val)) {
      Opc = RISCV::SH3ADD;
      Val = Val >> 3;
    } else if (isShiftedInt<12, 2>(Val)) {
      Opc = RISCV::SH2ADD;
      Val = Val >> 2;
    }
    if (Opc) {
      Register ScratchReg = MRI.createVirtualRegister(&RISCV::GPRRegClass);
      TII->movImm(MBB, II, DL, ScratchReg, Val, Flag);
      BuildMI(MBB, II, DL, TII->get(Opc), DestReg)
          .addReg(ScratchReg, RegState::Kill)
          .addReg(SrcReg, getKillRegState(KillSrcReg))
          .setMIFlag(Flag);
      return;
    }
  }

  unsigned Opc = RISCV::ADD;
  if (Val < 0) {
    Val = -Val;
    Opc = RISCV::SUB;
  }

  Register ScratchReg = MRI.createVirtualRegister(&RISCV::GPRRegClass);
  TII->movImm(MBB, II, DL, ScratchReg, Val, Flag);
  BuildMI(MBB, II, DL, TII->get(Opc), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrcReg))
      .addReg(ScratchReg, RegState::Kill)
      .setMIFlag(Flag);
}

// Split a VSPILLx_Mx pseudo into multiple whole register stores separated by
// LMUL*VLENB bytes.
void RISCVRegisterInfo::lowerVSPILL(MachineBasicBlock::iterator II) const {
  DebugLoc DL = II->getDebugLoc();
  MachineBasicBlock &MBB = *II->getParent();
  MachineFunction &MF = *MBB.getParent();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const RISCVSubtarget &STI = MF.getSubtarget<RISCVSubtarget>();
  const TargetInstrInfo *TII = STI.getInstrInfo();
  const TargetRegisterInfo *TRI = STI.getRegisterInfo();

  auto ZvlssegInfo = RISCV::isRVVSpillForZvlsseg(II->getOpcode());
  unsigned NF = ZvlssegInfo->first;
  unsigned LMUL = ZvlssegInfo->second;
  assert(NF * LMUL <= 8 && "Invalid NF/LMUL combinations.");
  unsigned Opcode, SubRegIdx;
  switch (LMUL) {
  default:
    llvm_unreachable("LMUL must be 1, 2, or 4.");
  case 1:
    Opcode = RISCV::VS1R_V;
    SubRegIdx = RISCV::sub_vrm1_0;
    break;
  case 2:
    Opcode = RISCV::VS2R_V;
    SubRegIdx = RISCV::sub_vrm2_0;
    break;
  case 4:
    Opcode = RISCV::VS4R_V;
    SubRegIdx = RISCV::sub_vrm4_0;
    break;
  }
  static_assert(RISCV::sub_vrm1_7 == RISCV::sub_vrm1_0 + 7,
                "Unexpected subreg numbering");
  static_assert(RISCV::sub_vrm2_3 == RISCV::sub_vrm2_0 + 3,
                "Unexpected subreg numbering");
  static_assert(RISCV::sub_vrm4_1 == RISCV::sub_vrm4_0 + 1,
                "Unexpected subreg numbering");

  Register VL = MRI.createVirtualRegister(&RISCV::GPRRegClass);
  // Optimize for constant VLEN.
  if (auto VLEN = STI.getRealVLen()) {
    const int64_t VLENB = *VLEN / 8;
    int64_t Offset = VLENB * LMUL;
    STI.getInstrInfo()->movImm(MBB, II, DL, VL, Offset);
  } else {
    BuildMI(MBB, II, DL, TII->get(RISCV::PseudoReadVLENB), VL);
    uint32_t ShiftAmount = Log2_32(LMUL);
    if (ShiftAmount != 0)
      BuildMI(MBB, II, DL, TII->get(RISCV::SLLI), VL)
          .addReg(VL)
          .addImm(ShiftAmount);
  }

  Register SrcReg = II->getOperand(0).getReg();
  Register Base = II->getOperand(1).getReg();
  bool IsBaseKill = II->getOperand(1).isKill();
  Register NewBase = MRI.createVirtualRegister(&RISCV::GPRRegClass);

  auto *OldMMO = *(II->memoperands_begin());
  LocationSize OldLoc = OldMMO->getSize();
  assert(OldLoc.isPrecise() && OldLoc.getValue().isKnownMultipleOf(NF));
  TypeSize NewSize = OldLoc.getValue().divideCoefficientBy(NF);
  auto *NewMMO = MF.getMachineMemOperand(OldMMO, OldMMO->getOffset(), NewSize);
  for (unsigned I = 0; I < NF; ++I) {
    // Adding implicit-use of super register to describe we are using part of
    // super register, that prevents machine verifier complaining when part of
    // subreg is undef, see comment in MachineVerifier::checkLiveness for more
    // detail.
    BuildMI(MBB, II, DL, TII->get(Opcode))
        .addReg(TRI->getSubReg(SrcReg, SubRegIdx + I))
        .addReg(Base, getKillRegState(I == NF - 1))
        .addMemOperand(NewMMO)
        .addReg(SrcReg, RegState::Implicit);
    if (I != NF - 1)
      BuildMI(MBB, II, DL, TII->get(RISCV::ADD), NewBase)
          .addReg(Base, getKillRegState(I != 0 || IsBaseKill))
          .addReg(VL, getKillRegState(I == NF - 2));
    Base = NewBase;
  }
  II->eraseFromParent();
}

// Split a VSPILLx_Mx pseudo into multiple whole register loads separated by
// LMUL*VLENB bytes.
void RISCVRegisterInfo::lowerVRELOAD(MachineBasicBlock::iterator II) const {
  DebugLoc DL = II->getDebugLoc();
  MachineBasicBlock &MBB = *II->getParent();
  MachineFunction &MF = *MBB.getParent();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const RISCVSubtarget &STI = MF.getSubtarget<RISCVSubtarget>();
  const TargetInstrInfo *TII = STI.getInstrInfo();
  const TargetRegisterInfo *TRI = STI.getRegisterInfo();

  auto ZvlssegInfo = RISCV::isRVVSpillForZvlsseg(II->getOpcode());
  unsigned NF = ZvlssegInfo->first;
  unsigned LMUL = ZvlssegInfo->second;
  assert(NF * LMUL <= 8 && "Invalid NF/LMUL combinations.");
  unsigned Opcode, SubRegIdx;
  switch (LMUL) {
  default:
    llvm_unreachable("LMUL must be 1, 2, or 4.");
  case 1:
    Opcode = RISCV::VL1RE8_V;
    SubRegIdx = RISCV::sub_vrm1_0;
    break;
  case 2:
    Opcode = RISCV::VL2RE8_V;
    SubRegIdx = RISCV::sub_vrm2_0;
    break;
  case 4:
    Opcode = RISCV::VL4RE8_V;
    SubRegIdx = RISCV::sub_vrm4_0;
    break;
  }
  static_assert(RISCV::sub_vrm1_7 == RISCV::sub_vrm1_0 + 7,
                "Unexpected subreg numbering");
  static_assert(RISCV::sub_vrm2_3 == RISCV::sub_vrm2_0 + 3,
                "Unexpected subreg numbering");
  static_assert(RISCV::sub_vrm4_1 == RISCV::sub_vrm4_0 + 1,
                "Unexpected subreg numbering");

  Register VL = MRI.createVirtualRegister(&RISCV::GPRRegClass);
  // Optimize for constant VLEN.
  if (auto VLEN = STI.getRealVLen()) {
    const int64_t VLENB = *VLEN / 8;
    int64_t Offset = VLENB * LMUL;
    STI.getInstrInfo()->movImm(MBB, II, DL, VL, Offset);
  } else {
    BuildMI(MBB, II, DL, TII->get(RISCV::PseudoReadVLENB), VL);
    uint32_t ShiftAmount = Log2_32(LMUL);
    if (ShiftAmount != 0)
      BuildMI(MBB, II, DL, TII->get(RISCV::SLLI), VL)
          .addReg(VL)
          .addImm(ShiftAmount);
  }

  Register DestReg = II->getOperand(0).getReg();
  Register Base = II->getOperand(1).getReg();
  bool IsBaseKill = II->getOperand(1).isKill();
  Register NewBase = MRI.createVirtualRegister(&RISCV::GPRRegClass);
  auto *OldMMO = *(II->memoperands_begin());
  LocationSize OldLoc = OldMMO->getSize();
  assert(OldLoc.isPrecise() && OldLoc.getValue().isKnownMultipleOf(NF));
  TypeSize NewSize = OldLoc.getValue().divideCoefficientBy(NF);
  auto *NewMMO = MF.getMachineMemOperand(OldMMO, OldMMO->getOffset(), NewSize);
  for (unsigned I = 0; I < NF; ++I) {
    BuildMI(MBB, II, DL, TII->get(Opcode),
            TRI->getSubReg(DestReg, SubRegIdx + I))
        .addReg(Base, getKillRegState(I == NF - 1))
        .addMemOperand(NewMMO);
    if (I != NF - 1)
      BuildMI(MBB, II, DL, TII->get(RISCV::ADD), NewBase)
          .addReg(Base, getKillRegState(I != 0 || IsBaseKill))
          .addReg(VL, getKillRegState(I == NF - 2));
    Base = NewBase;
  }
  II->eraseFromParent();
}

bool RISCVRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                            int SPAdj, unsigned FIOperandNum,
                                            RegScavenger *RS) const {
  assert(SPAdj == 0 && "Unexpected non-zero SPAdj value");

  MachineInstr &MI = *II;
  MachineFunction &MF = *MI.getParent()->getParent();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  DebugLoc DL = MI.getDebugLoc();

  int FrameIndex = MI.getOperand(FIOperandNum).getIndex();
  Register FrameReg;
  StackOffset Offset =
      getFrameLowering(MF)->getFrameIndexReference(MF, FrameIndex, FrameReg);
  bool IsRVVSpill = RISCV::isRVVSpill(MI);
  if (!IsRVVSpill)
    Offset += StackOffset::getFixed(MI.getOperand(FIOperandNum + 1).getImm());

  if (!isInt<32>(Offset.getFixed())) {
    report_fatal_error(
        "Frame offsets outside of the signed 32-bit range not supported");
  }

  if (!IsRVVSpill) {
    int64_t Val = Offset.getFixed();
    int64_t Lo12 = SignExtend64<12>(Val);
    unsigned Opc = MI.getOpcode();
    if (Opc == RISCV::ADDI && !isInt<12>(Val)) {
      // We chose to emit the canonical immediate sequence rather than folding
      // the offset into the using add under the theory that doing so doesn't
      // save dynamic instruction count and some target may fuse the canonical
      // 32 bit immediate sequence.  We still need to clear the portion of the
      // offset encoded in the immediate.
      MI.getOperand(FIOperandNum + 1).ChangeToImmediate(0);
    } else if ((Opc == RISCV::PREFETCH_I || Opc == RISCV::PREFETCH_R ||
                Opc == RISCV::PREFETCH_W) &&
               (Lo12 & 0b11111) != 0) {
      // Prefetch instructions require the offset to be 32 byte aligned.
      MI.getOperand(FIOperandNum + 1).ChangeToImmediate(0);
    } else if ((Opc == RISCV::PseudoRV32ZdinxLD ||
                Opc == RISCV::PseudoRV32ZdinxSD) &&
               Lo12 >= 2044) {
      // This instruction will be split into 2 instructions. The second
      // instruction will add 4 to the immediate. If that would overflow 12
      // bits, we can't fold the offset.
      MI.getOperand(FIOperandNum + 1).ChangeToImmediate(0);
    } else {
      // We can encode an add with 12 bit signed immediate in the immediate
      // operand of our user instruction.  As a result, the remaining
      // offset can by construction, at worst, a LUI and a ADD.
      MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Lo12);
      Offset = StackOffset::get((uint64_t)Val - (uint64_t)Lo12,
                                Offset.getScalable());
    }
  }

  if (Offset.getScalable() || Offset.getFixed()) {
    Register DestReg;
    if (MI.getOpcode() == RISCV::ADDI)
      DestReg = MI.getOperand(0).getReg();
    else
      DestReg = MRI.createVirtualRegister(&RISCV::GPRRegClass);
    adjustReg(*II->getParent(), II, DL, DestReg, FrameReg, Offset,
              MachineInstr::NoFlags, std::nullopt);
    MI.getOperand(FIOperandNum).ChangeToRegister(DestReg, /*IsDef*/false,
                                                 /*IsImp*/false,
                                                 /*IsKill*/true);
  } else {
    MI.getOperand(FIOperandNum).ChangeToRegister(FrameReg, /*IsDef*/false,
                                                 /*IsImp*/false,
                                                 /*IsKill*/false);
  }

  // If after materializing the adjustment, we have a pointless ADDI, remove it
  if (MI.getOpcode() == RISCV::ADDI &&
      MI.getOperand(0).getReg() == MI.getOperand(1).getReg() &&
      MI.getOperand(2).getImm() == 0) {
    MI.eraseFromParent();
    return true;
  }

  // Handle spill/fill of synthetic register classes for segment operations to
  // ensure correctness in the edge case one gets spilled. There are many
  // possible optimizations here, but given the extreme rarity of such spills,
  // we prefer simplicity of implementation for now.
  switch (MI.getOpcode()) {
  case RISCV::PseudoVSPILL2_M1:
  case RISCV::PseudoVSPILL2_M2:
  case RISCV::PseudoVSPILL2_M4:
  case RISCV::PseudoVSPILL3_M1:
  case RISCV::PseudoVSPILL3_M2:
  case RISCV::PseudoVSPILL4_M1:
  case RISCV::PseudoVSPILL4_M2:
  case RISCV::PseudoVSPILL5_M1:
  case RISCV::PseudoVSPILL6_M1:
  case RISCV::PseudoVSPILL7_M1:
  case RISCV::PseudoVSPILL8_M1:
    lowerVSPILL(II);
    return true;
  case RISCV::PseudoVRELOAD2_M1:
  case RISCV::PseudoVRELOAD2_M2:
  case RISCV::PseudoVRELOAD2_M4:
  case RISCV::PseudoVRELOAD3_M1:
  case RISCV::PseudoVRELOAD3_M2:
  case RISCV::PseudoVRELOAD4_M1:
  case RISCV::PseudoVRELOAD4_M2:
  case RISCV::PseudoVRELOAD5_M1:
  case RISCV::PseudoVRELOAD6_M1:
  case RISCV::PseudoVRELOAD7_M1:
  case RISCV::PseudoVRELOAD8_M1:
    lowerVRELOAD(II);
    return true;
  }

  return false;
}

bool RISCVRegisterInfo::requiresVirtualBaseRegisters(
    const MachineFunction &MF) const {
  return true;
}

// Returns true if the instruction's frame index reference would be better
// served by a base register other than FP or SP.
// Used by LocalStackSlotAllocation pass to determine which frame index
// references it should create new base registers for.
bool RISCVRegisterInfo::needsFrameBaseReg(MachineInstr *MI,
                                          int64_t Offset) const {
  unsigned FIOperandNum = 0;
  for (; !MI->getOperand(FIOperandNum).isFI(); FIOperandNum++)
    assert(FIOperandNum < MI->getNumOperands() &&
           "Instr doesn't have FrameIndex operand");

  // For RISC-V, The machine instructions that include a FrameIndex operand
  // are load/store, ADDI instructions.
  unsigned MIFrm = RISCVII::getFormat(MI->getDesc().TSFlags);
  if (MIFrm != RISCVII::InstFormatI && MIFrm != RISCVII::InstFormatS)
    return false;
  // We only generate virtual base registers for loads and stores, so
  // return false for everything else.
  if (!MI->mayLoad() && !MI->mayStore())
    return false;

  const MachineFunction &MF = *MI->getMF();
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const RISCVFrameLowering *TFI = getFrameLowering(MF);
  const MachineRegisterInfo &MRI = MF.getRegInfo();

  if (TFI->hasFP(MF) && !shouldRealignStack(MF)) {
    auto &Subtarget = MF.getSubtarget<RISCVSubtarget>();
    // Estimate the stack size used to store callee saved registers(
    // excludes reserved registers).
    unsigned CalleeSavedSize = 0;
    for (const MCPhysReg *R = MRI.getCalleeSavedRegs(); MCPhysReg Reg = *R;
         ++R) {
      if (Subtarget.isRegisterReservedByUser(Reg))
        continue;

      if (RISCV::GPRRegClass.contains(Reg))
        CalleeSavedSize += getSpillSize(RISCV::GPRRegClass);
      else if (RISCV::FPR64RegClass.contains(Reg))
        CalleeSavedSize += getSpillSize(RISCV::FPR64RegClass);
      else if (RISCV::FPR32RegClass.contains(Reg))
        CalleeSavedSize += getSpillSize(RISCV::FPR32RegClass);
      // Ignore vector registers.
    }

    int64_t MaxFPOffset = Offset - CalleeSavedSize;
    return !isFrameOffsetLegal(MI, RISCV::X8, MaxFPOffset);
  }

  // Assume 128 bytes spill slots size to estimate the maximum possible
  // offset relative to the stack pointer.
  // FIXME: The 128 is copied from ARM. We should run some statistics and pick a
  // real one for RISC-V.
  int64_t MaxSPOffset = Offset + 128;
  MaxSPOffset += MFI.getLocalFrameSize();
  return !isFrameOffsetLegal(MI, RISCV::X2, MaxSPOffset);
}

// Determine whether a given base register plus offset immediate is
// encodable to resolve a frame index.
bool RISCVRegisterInfo::isFrameOffsetLegal(const MachineInstr *MI,
                                           Register BaseReg,
                                           int64_t Offset) const {
  unsigned FIOperandNum = 0;
  while (!MI->getOperand(FIOperandNum).isFI()) {
    FIOperandNum++;
    assert(FIOperandNum < MI->getNumOperands() &&
           "Instr does not have a FrameIndex operand!");
  }

  Offset += getFrameIndexInstrOffset(MI, FIOperandNum);
  return isInt<12>(Offset);
}

// Insert defining instruction(s) for a pointer to FrameIdx before
// insertion point I.
// Return materialized frame pointer.
Register RISCVRegisterInfo::materializeFrameBaseRegister(MachineBasicBlock *MBB,
                                                         int FrameIdx,
                                                         int64_t Offset) const {
  MachineBasicBlock::iterator MBBI = MBB->begin();
  DebugLoc DL;
  if (MBBI != MBB->end())
    DL = MBBI->getDebugLoc();
  MachineFunction *MF = MBB->getParent();
  MachineRegisterInfo &MFI = MF->getRegInfo();
  const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();

  Register BaseReg = MFI.createVirtualRegister(&RISCV::GPRRegClass);
  BuildMI(*MBB, MBBI, DL, TII->get(RISCV::ADDI), BaseReg)
      .addFrameIndex(FrameIdx)
      .addImm(Offset);
  return BaseReg;
}

// Resolve a frame index operand of an instruction to reference the
// indicated base register plus offset instead.
void RISCVRegisterInfo::resolveFrameIndex(MachineInstr &MI, Register BaseReg,
                                          int64_t Offset) const {
  unsigned FIOperandNum = 0;
  while (!MI.getOperand(FIOperandNum).isFI()) {
    FIOperandNum++;
    assert(FIOperandNum < MI.getNumOperands() &&
           "Instr does not have a FrameIndex operand!");
  }

  Offset += getFrameIndexInstrOffset(&MI, FIOperandNum);
  // FrameIndex Operands are always represented as a
  // register followed by an immediate.
  MI.getOperand(FIOperandNum).ChangeToRegister(BaseReg, false);
  MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
}

// Get the offset from the referenced frame index in the instruction,
// if there is one.
int64_t RISCVRegisterInfo::getFrameIndexInstrOffset(const MachineInstr *MI,
                                                    int Idx) const {
  assert((RISCVII::getFormat(MI->getDesc().TSFlags) == RISCVII::InstFormatI ||
          RISCVII::getFormat(MI->getDesc().TSFlags) == RISCVII::InstFormatS) &&
         "The MI must be I or S format.");
  assert(MI->getOperand(Idx).isFI() && "The Idx'th operand of MI is not a "
                                       "FrameIndex operand");
  return MI->getOperand(Idx + 1).getImm();
}

Register RISCVRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  const TargetFrameLowering *TFI = getFrameLowering(MF);
  return TFI->hasFP(MF) ? RISCV::X8 : RISCV::X2;
}

StringRef RISCVRegisterInfo::getRegAsmName(MCRegister Reg) const {
  if (Reg == RISCV::SF_VCIX_STATE)
    return "sf.vcix_state";
  return TargetRegisterInfo::getRegAsmName(Reg);
}

namespace {
// XAIcore 的 tile/acc/vec 寄存器是“私有寄存器文件”：普通函数（libgcc 的
// 软浮点辅助函数等）不使用它们。把它们加入 call-preserved mask 后，调用点
// 不再把它们标成 clobbered，NPU 值可以跨 call 存活。
//
// 契约：所有使用 tile/acc/vec 的函数必须内联；外部函数（libgcc、scalar
// helper）不得使用这些寄存器。违反契约会导致 NPU 值被 callee 破坏。
const uint32_t *withXAIcorePreserved(const uint32_t *Base) {
  static std::map<const uint32_t *, std::vector<uint32_t>> Cache;
  auto It = Cache.find(Base);
  if (It != Cache.end())
    return It->second.data();
  std::vector<uint32_t> Mask((RISCV::NUM_TARGET_REGS + 31) / 32);
  std::copy(Base, Base + Mask.size(), Mask.begin());
  auto Add = [&](const TargetRegisterClass &RC) {
    for (MCPhysReg R : RC)
      Mask[R / 32] |= 1u << (R % 32);
  };
  Add(RISCV::XTTRegClass);
  Add(RISCV::XTARegClass);
  Add(RISCV::XTVRegClass);
  Add(RISCV::XTBRegClass);
  auto Res = Cache.emplace(Base, std::move(Mask));
  return Res.first->second.data();
}
} // namespace

const uint32_t *
RISCVRegisterInfo::getCallPreservedMask(const MachineFunction & MF,
                                        CallingConv::ID CC) const {
  auto &Subtarget = MF.getSubtarget<RISCVSubtarget>();

  if (CC == CallingConv::GHC)
    return CSR_NoRegs_RegMask;
  RISCVABI::ABI ABI = Subtarget.getTargetABI();
  if (CC == CallingConv::PreserveMost) {
    if (ABI == RISCVABI::ABI_ILP32E || ABI == RISCVABI::ABI_LP64E)
      return CSR_RT_MostRegs_RVE_RegMask;
    return CSR_RT_MostRegs_RegMask;
  }
  switch (ABI) {
  default:
    llvm_unreachable("Unrecognized ABI");
  case RISCVABI::ABI_ILP32E:
  case RISCVABI::ABI_LP64E:
    return withXAIcorePreserved(CSR_ILP32E_LP64E_RegMask);
  case RISCVABI::ABI_ILP32:
  case RISCVABI::ABI_LP64:
    if (CC == CallingConv::RISCV_VectorCall)
      return withXAIcorePreserved(CSR_ILP32_LP64_V_RegMask);
    return withXAIcorePreserved(CSR_ILP32_LP64_RegMask);
  case RISCVABI::ABI_ILP32F:
  case RISCVABI::ABI_LP64F:
    if (CC == CallingConv::RISCV_VectorCall)
      return withXAIcorePreserved(CSR_ILP32F_LP64F_V_RegMask);
    return withXAIcorePreserved(CSR_ILP32F_LP64F_RegMask);
  case RISCVABI::ABI_ILP32D:
  case RISCVABI::ABI_LP64D:
    if (CC == CallingConv::RISCV_VectorCall)
      return withXAIcorePreserved(CSR_ILP32D_LP64D_V_RegMask);
    return withXAIcorePreserved(CSR_ILP32D_LP64D_RegMask);
  }
}

const TargetRegisterClass *
RISCVRegisterInfo::getLargestLegalSuperClass(const TargetRegisterClass *RC,
                                             const MachineFunction &) const {
  if (RC == &RISCV::VMV0RegClass)
    return &RISCV::VRRegClass;
  if (RC == &RISCV::VRNoV0RegClass)
    return &RISCV::VRRegClass;
  if (RC == &RISCV::VRM2NoV0RegClass)
    return &RISCV::VRM2RegClass;
  if (RC == &RISCV::VRM4NoV0RegClass)
    return &RISCV::VRM4RegClass;
  if (RC == &RISCV::VRM8NoV0RegClass)
    return &RISCV::VRM8RegClass;
  return RC;
}

void RISCVRegisterInfo::getOffsetOpcodes(const StackOffset &Offset,
                                         SmallVectorImpl<uint64_t> &Ops) const {
  // VLENB is the length of a vector register in bytes. We use <vscale x 8 x i8>
  // to represent one vector register. The dwarf offset is
  // VLENB * scalable_offset / 8.
  assert(Offset.getScalable() % 8 == 0 && "Invalid frame offset");

  // Add fixed-sized offset using existing DIExpression interface.
  DIExpression::appendOffset(Ops, Offset.getFixed());

  unsigned VLENB = getDwarfRegNum(RISCV::VLENB, true);
  int64_t VLENBSized = Offset.getScalable() / 8;
  if (VLENBSized > 0) {
    Ops.push_back(dwarf::DW_OP_constu);
    Ops.push_back(VLENBSized);
    Ops.append({dwarf::DW_OP_bregx, VLENB, 0ULL});
    Ops.push_back(dwarf::DW_OP_mul);
    Ops.push_back(dwarf::DW_OP_plus);
  } else if (VLENBSized < 0) {
    Ops.push_back(dwarf::DW_OP_constu);
    Ops.push_back(-VLENBSized);
    Ops.append({dwarf::DW_OP_bregx, VLENB, 0ULL});
    Ops.push_back(dwarf::DW_OP_mul);
    Ops.push_back(dwarf::DW_OP_minus);
  }
}

unsigned
RISCVRegisterInfo::getRegisterCostTableIndex(const MachineFunction &MF) const {
  return MF.getSubtarget<RISCVSubtarget>().hasStdExtZca() && !DisableCostPerUse
             ? 1
             : 0;
}

float RISCVRegisterInfo::getSpillWeightScaleFactor(
    const TargetRegisterClass *RC) const {
  return getRegClassWeight(RC).RegWeight;
}

// XAIcore 的四个寄存器文件（tile/acc/vec32/vec8）都是**单端口双 bank**：
// 每个类的前一半是一个 bank、后一半是另一个 bank。任何"同一条指令要读两个
// 同类寄存器"的运算（mma 的 A/B、vec 的 vmax/vadd、acc 的 aadd/adiv……），
// 只有两个操作数落在不同 bank 才能一拍读出；落在同一 bank 仍然正确，但要
// 串行读两次（慢约一倍）。所以 RA 阶段尽量把它们分开。
static cl::opt<bool> XAIcoreBankHints(
    "riscv-xaicore-bank-hints", cl::Hidden, cl::init(true),
    cl::desc("XAIcore: hint paired register operands into different register banks"));

namespace {
/// 寄存器在其类里的 bank（前半 = 0，后半 = 1）。
unsigned xaicoreBank(MCPhysReg Reg, const TargetRegisterClass &RC) {
  const unsigned Half = RC.getNumRegs() / 2;
  unsigned Idx = 0;
  for (MCPhysReg R : RC) {
    if (R == Reg)
      return Idx >= Half ? 1 : 0;
    ++Idx;
  }
  llvm_unreachable("register not in class");
}

/// 是否是会同时读两个 tile 操作数的 mma。
bool isXAIcoreMMA(unsigned Opcode) {
  switch (Opcode) {
  default:
    return false;
  case RISCV::XAIcoreMMANNZero:
  case RISCV::XAIcoreMMANNAcc:
  case RISCV::XAIcoreMMANTZero:
  case RISCV::XAIcoreMMANTAcc:
    return true;
  }
}

/// 是否是 XAIcore 的四个"可分 bank"寄存器类之一。
bool isXAIcoreBankedClass(const TargetRegisterClass &RC) {
  return RISCV::XTTRegClass.hasSubClassEq(&RC) ||
         RISCV::XTARegClass.hasSubClassEq(&RC) ||
         RISCV::XTVRegClass.hasSubClassEq(&RC) ||
         RISCV::XTBRegClass.hasSubClassEq(&RC);
}
} // namespace

/// 把某个 bank 内的寄存器按 Order 顺序追加成 hint（软 hint，满了就回退）。
static void addXAIcoreBankHints(const TargetRegisterClass &RC, unsigned Bank,
                                ArrayRef<MCPhysReg> Order,
                                SmallVectorImpl<MCPhysReg> &Hints) {
  const unsigned Half = RC.getNumRegs() / 2;
  unsigned Idx = 0;
  for (MCPhysReg Reg : RC) {
    const unsigned RegBank = Idx++ >= Half ? 1 : 0;
    if (RegBank == Bank && is_contained(Order, Reg) && !is_contained(Hints, Reg))
      Hints.push_back(Reg);
  }
}

// Add two address hints to improve chances of being able to use a compressed
// instruction.
bool RISCVRegisterInfo::getRegAllocationHints(
    Register VirtReg, ArrayRef<MCPhysReg> Order,
    SmallVectorImpl<MCPhysReg> &Hints, const MachineFunction &MF,
    const VirtRegMap *VRM, const LiveRegMatrix *Matrix) const {
  const MachineRegisterInfo *MRI = &MF.getRegInfo();
  auto &Subtarget = MF.getSubtarget<RISCVSubtarget>();

  bool BaseImplRetVal = TargetRegisterInfo::getRegAllocationHints(
      VirtReg, Order, Hints, MF, VRM, Matrix);

  if (!VRM || DisableRegAllocHints)
    return BaseImplRetVal;

  // XAIcore：把"同一条指令里读到的两个同类寄存器"尽量分到不同 bank。
  //
  // 优先处理 tile 的 mma，并且**方向固定**为 A 走低 bank、B 走高 bank：GEMM 的
  // 一组正好是 1 个 A + 8 个 B，而高 bank 恰好 8 个寄存器，所以这一组 8 条
  // mma 能全部并行读。其余情况（vec32 的 vmax/vadd、acc 的 aadd/adiv、以及
  // 任何两位同类寄存器的指令）用通用规则：如果对方已经分配，就提示相反的
  // bank；对方还没分配则不猜（避免把方向搞反）。
  //
  // 都是**软 hint**：目标 bank 满了就回退到普通分配顺序，绝不会导致分配失败。
  //
  // 注意：VirtReg 是虚拟寄存器，RC->contains() 只判定物理寄存器，所以这里
  // 一律用 hasSubClassEq(getRegClass(VirtReg)) 判类。
  if (XAIcoreBankHints && VirtReg.isVirtual()) {
    const TargetRegisterClass *RC = MRI->getRegClass(VirtReg);
    if (isXAIcoreBankedClass(*RC)) {
      // ---- ① tile 的 mma：固定方向 ----
      bool Oriented = false;
      if (RISCV::XTTRegClass.hasSubClassEq(RC)) {
        for (const MachineInstr &MI : MRI->reg_nodbg_instructions(VirtReg)) {
          if (!isXAIcoreMMA(MI.getOpcode()))
            continue;
          // mma 的操作数：0 = acc(XTA)，1 = A(tile)，2 = B(tile)。
          unsigned OpNo = 0;
          for (unsigned I = 0, E = MI.getNumOperands(); I != E; ++I) {
            const MachineOperand &MO = MI.getOperand(I);
            if (MO.isReg() && MO.getReg() == VirtReg) {
              OpNo = I;
              break;
            }
          }
          if (OpNo != 1 && OpNo != 2)
            continue;   // 不是 A/B（例如它是 acc），交给通用规则
          Register OtherReg = MI.getOperand(OpNo == 1 ? 2 : 1).getReg();
          unsigned OtherBank = 0;
          bool Known = false;
          if (OtherReg.isPhysical()) {
            OtherBank = xaicoreBank(OtherReg, RISCV::XTTRegClass);
            Known = true;
          } else if (Register P = VRM->getPhys(OtherReg)) {
            OtherBank = xaicoreBank(P, RISCV::XTTRegClass);
            Known = true;
          }
          // A(1) 走低 bank、B(2) 走高 bank；若对方已分配则取它的反面。
          addXAIcoreBankHints(RISCV::XTTRegClass,
                              Known ? 1 - OtherBank : (OpNo == 1 ? 0 : 1), Order,
                              Hints);
          Oriented = true;
          break;
        }
      }
      // ---- ② 通用：任何"同指令读两个同类寄存器"的场合 ----
      if (!Oriented) {
        for (const MachineInstr &MI : MRI->reg_nodbg_instructions(VirtReg)) {
          Register Other;
          bool Found = false, Ambiguous = false;
          for (unsigned I = 0, E = MI.getNumOperands(); I != E; ++I) {
            const MachineOperand &MO = MI.getOperand(I);
            if (!MO.isReg())
              continue;
            Register R = MO.getReg();
            if (R == VirtReg)
              continue;
            if (R.isPhysical() ? !RC->contains(R)
                               : !RC->hasSubClassEq(MRI->getRegClass(R)))
              continue;   // 不同类，不构成 bank 冲突
            if (Found) {  // 三个以上同类操作数：无法两两分开，放弃
              Ambiguous = true;
              break;
            }
            Other = R;
            Found = true;
          }
          if (!Found || Ambiguous)
            continue;
          Register OtherPhys =
              Other.isPhysical() ? Other : Register(VRM->getPhys(Other));
          if (!OtherPhys)
            continue;   // 对方还没分配 → 不猜
          addXAIcoreBankHints(*RC, 1 - xaicoreBank(OtherPhys, *RC), Order, Hints);
          break;
        }
      }
    }
  }

  // Add any two address hints after any copy hints.
  SmallSet<Register, 4> TwoAddrHints;

  auto tryAddHint = [&](const MachineOperand &VRRegMO, const MachineOperand &MO,
                        bool NeedGPRC) -> void {
    Register Reg = MO.getReg();
    Register PhysReg = Reg.isPhysical() ? Reg : Register(VRM->getPhys(Reg));
    // TODO: Support GPRPair subregisters? Need to be careful with even/odd
    // registers. If the virtual register is an odd register of a pair and the
    // physical register is even (or vice versa), we should not add the hint.
    if (PhysReg && (!NeedGPRC || RISCV::GPRCRegClass.contains(PhysReg)) &&
        !MO.getSubReg() && !VRRegMO.getSubReg()) {
      if (!MRI->isReserved(PhysReg) && !is_contained(Hints, PhysReg))
        TwoAddrHints.insert(PhysReg);
    }
  };

  // This is all of the compressible binary instructions. If an instruction
  // needs GPRC register class operands \p NeedGPRC will be set to true.
  auto isCompressible = [&Subtarget](const MachineInstr &MI, bool &NeedGPRC) {
    NeedGPRC = false;
    switch (MI.getOpcode()) {
    default:
      return false;
    case RISCV::AND:
    case RISCV::OR:
    case RISCV::XOR:
    case RISCV::SUB:
    case RISCV::ADDW:
    case RISCV::SUBW:
      NeedGPRC = true;
      return true;
    case RISCV::ANDI: {
      NeedGPRC = true;
      if (!MI.getOperand(2).isImm())
        return false;
      int64_t Imm = MI.getOperand(2).getImm();
      if (isInt<6>(Imm))
        return true;
      // c.zext.b
      return Subtarget.hasStdExtZcb() && Imm == 255;
    }
    case RISCV::SRAI:
    case RISCV::SRLI:
      NeedGPRC = true;
      return true;
    case RISCV::ADD:
    case RISCV::SLLI:
      return true;
    case RISCV::ADDI:
    case RISCV::ADDIW:
      return MI.getOperand(2).isImm() && isInt<6>(MI.getOperand(2).getImm());
    case RISCV::MUL:
    case RISCV::SEXT_B:
    case RISCV::SEXT_H:
    case RISCV::ZEXT_H_RV32:
    case RISCV::ZEXT_H_RV64:
      // c.mul, c.sext.b, c.sext.h, c.zext.h
      NeedGPRC = true;
      return Subtarget.hasStdExtZcb();
    case RISCV::ADD_UW:
      // c.zext.w
      NeedGPRC = true;
      return Subtarget.hasStdExtZcb() && MI.getOperand(2).isReg() &&
             MI.getOperand(2).getReg() == RISCV::X0;
    case RISCV::XORI:
      // c.not
      NeedGPRC = true;
      return Subtarget.hasStdExtZcb() && MI.getOperand(2).isImm() &&
             MI.getOperand(2).getImm() == -1;
    }
  };

  // Returns true if this operand is compressible. For non-registers it always
  // returns true. Immediate range was already checked in isCompressible.
  // For registers, it checks if the register is a GPRC register. reg-reg
  // instructions that require GPRC need all register operands to be GPRC.
  auto isCompressibleOpnd = [&](const MachineOperand &MO) {
    if (!MO.isReg())
      return true;
    Register Reg = MO.getReg();
    Register PhysReg = Reg.isPhysical() ? Reg : Register(VRM->getPhys(Reg));
    return PhysReg && RISCV::GPRCRegClass.contains(PhysReg);
  };

  for (auto &MO : MRI->reg_nodbg_operands(VirtReg)) {
    const MachineInstr &MI = *MO.getParent();
    unsigned OpIdx = MO.getOperandNo();
    bool NeedGPRC;
    if (isCompressible(MI, NeedGPRC)) {
      if (OpIdx == 0 && MI.getOperand(1).isReg()) {
        if (!NeedGPRC || MI.getNumExplicitOperands() < 3 ||
            MI.getOpcode() == RISCV::ADD_UW ||
            isCompressibleOpnd(MI.getOperand(2)))
          tryAddHint(MO, MI.getOperand(1), NeedGPRC);
        if (MI.isCommutable() && MI.getOperand(2).isReg() &&
            (!NeedGPRC || isCompressibleOpnd(MI.getOperand(1))))
          tryAddHint(MO, MI.getOperand(2), NeedGPRC);
      } else if (OpIdx == 1 && (!NeedGPRC || MI.getNumExplicitOperands() < 3 ||
                                isCompressibleOpnd(MI.getOperand(2)))) {
        tryAddHint(MO, MI.getOperand(0), NeedGPRC);
      } else if (MI.isCommutable() && OpIdx == 2 &&
                 (!NeedGPRC || isCompressibleOpnd(MI.getOperand(1)))) {
        tryAddHint(MO, MI.getOperand(0), NeedGPRC);
      }
    }

    // Add a hint if it would allow auipc/lui+addi(w) fusion.
    if ((MI.getOpcode() == RISCV::ADDIW || MI.getOpcode() == RISCV::ADDI) &&
        MI.getOperand(1).isReg()) {
      const MachineBasicBlock &MBB = *MI.getParent();
      MachineBasicBlock::const_iterator I = MI.getIterator();
      // Is the previous instruction a LUI or AUIPC that can be fused?
      if (I != MBB.begin()) {
        I = skipDebugInstructionsBackward(std::prev(I), MBB.begin());
        if (((I->getOpcode() == RISCV::LUI && Subtarget.hasLUIADDIFusion()) ||
             (I->getOpcode() == RISCV::AUIPC &&
              Subtarget.hasAUIPCADDIFusion())) &&
            I->getOperand(0).getReg() == MI.getOperand(1).getReg()) {
          if (OpIdx == 0)
            tryAddHint(MO, MI.getOperand(1), /*NeedGPRC=*/false);
          else
            tryAddHint(MO, MI.getOperand(0), /*NeedGPRC=*/false);
        }
      }
    }
  }

  for (MCPhysReg OrderReg : Order)
    if (TwoAddrHints.count(OrderReg))
      Hints.push_back(OrderReg);

  return BaseImplRetVal;
}

bool RISCVRegisterInfo::shouldRegionSplitForVirtReg(
    const MachineFunction &MF, const LiveInterval &VirtReg) const {
  if (!VirtReg.reg().isVirtual())
    return true;
  const TargetRegisterClass *RC = MF.getRegInfo().getRegClass(VirtReg.reg());
  if (RISCV::XTTRegClass.hasSubClassEq(RC) ||
      RISCV::XTARegClass.hasSubClassEq(RC) ||
      RISCV::XTVRegClass.hasSubClassEq(RC) ||
      RISCV::XTBRegClass.hasSubClassEq(RC))
    return false;
  return true;
}

bool RISCVRegisterInfo::isSpillableForVirtReg(
    const MachineFunction &MF, const LiveInterval &VirtReg) const {
  if (!VirtReg.reg().isVirtual())
    return true;
  const TargetRegisterClass *RC = MF.getRegInfo().getRegClass(VirtReg.reg());
  if (RISCV::XTTRegClass.hasSubClassEq(RC) ||
      RISCV::XTARegClass.hasSubClassEq(RC) ||
      RISCV::XTVRegClass.hasSubClassEq(RC) ||
      RISCV::XTBRegClass.hasSubClassEq(RC))
    return false;
  return true;
}
