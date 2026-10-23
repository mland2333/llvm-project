//===-- RISCVInstrInfoXAIcore.td - Tile NPU (YAI) -----------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tile NPU / yai-isa v1.0 扩展。
//
// 从第四步开始，Tile 不再是固定的 t0，而是 LLVM 寄存器分配器管理的值：
//
//   * Tile 值在 IR 里的载体是 <1024 x i8>（32x32 个 i8 = 8192 bit）；
//   * 后端的 XTT 寄存器类有 16 个物理 Tile 寄存器 t0..t15；
//   * cfg.setx / cfg.get / tload / tstore 的 Tile 操作数来自寄存器分配，
//     不再由程序员或固定文本决定；
//   * 编号与配置寄存器共享：分配到 t3 就用 tc3/tm3（编码天然一致）。
//
// MC 名称用 xttN，避免与 GPR 的 ABI 别名 tN 冲突；汇编文本由 PrintMethod
// 输出 tN / tcN / tmN。
//
// 指令编码与 fixed_t0_lower.py、npucc/assembler.py 以及 yai-isa v1.0 一致：
//
//   cfg.setx: major=0b0111, sel[2], vec, 0b000, field[5], rs1[5], idx[5], 0b10
//   cfg.get : major=0b0111, sel[2], vec, 0b010, field[5], idx[5], rd[5],  0b10
//   tload   : major=0, f=0, rs1[5], idx[5], 0b01
//   tstore  : major=0, f=1, rs1[5], idx[5], 0b01
//   tcopy   : major=0b0010, 0b010, rs1[5], idx[5], 0b01
//
// 其中 tc=0, ac=1, tm=2, am=3（sel 位于 bit 27-26），vec 位于 bit 25；
// tc 的 field 为 dtype/rows/cols/layout/arith_mode = 0..4，tm 的 field
// 为 base_addr/row_stride_bytes/col_stride_bytes/transform/flags = 0..4。
//
//===----------------------------------------------------------------------===//

// LLVM 侧把 Tile NPU 扩展命名为 xaicore；助记符保持 yai-isa v1.0 的
// cfg.setx / cfg.get / tload / tstore。
//
// 用 RISCVExtension 而不是裸 SubtargetFeature：这样 -march=rv64im_xaicore
// 会像其它 vendor extension 一样被 TargetParser 识别，同时 -mattr=+xaicore
// 也仍然可用（llc / llvm-mc 的开发期路径）。
def FeatureVendorXAIcore : RISCVExtension<1, 0, "YAI Tile NPU extension">;

def HasVendorXAIcore
    : Predicate<"Subtarget->hasVendorXAIcore()">,
      AssemblerPredicate<(all_of FeatureVendorXAIcore), "XAIcore">;

//===----------------------------------------------------------------------===//
// Tile 数据寄存器（XTT0..XTT15）
//===----------------------------------------------------------------------===//

def XTT0 : RISCVReg<0, "xtt0">;
def XTT1 : RISCVReg<1, "xtt1">;
def XTT2 : RISCVReg<2, "xtt2">;
def XTT3 : RISCVReg<3, "xtt3">;
def XTT4 : RISCVReg<4, "xtt4">;
def XTT5 : RISCVReg<5, "xtt5">;
def XTT6 : RISCVReg<6, "xtt6">;
def XTT7 : RISCVReg<7, "xtt7">;
def XTT8 : RISCVReg<8, "xtt8">;
def XTT9 : RISCVReg<9, "xtt9">;
def XTT10 : RISCVReg<10, "xtt10">;
def XTT11 : RISCVReg<11, "xtt11">;
def XTT12 : RISCVReg<12, "xtt12">;
def XTT13 : RISCVReg<13, "xtt13">;
def XTT14 : RISCVReg<14, "xtt14">;
def XTT15 : RISCVReg<15, "xtt15">;

// 一个 Tile 是 32x32 个 i8，共 8192 bit。
def XTT : RegisterClass<"RISCV", [v1024i8], 8192, (sequence "XTT%u", 0, 15)> {
  let Size = 8192;
}

def XTTOp : RegisterOperand<XTT> {
  let PrintMethod = "printXAIcoreTileReg";
}
def XTTCOp : RegisterOperand<XTT> {
  let PrintMethod = "printXAIcoreTileConfig";
}
def XTTMOp : RegisterOperand<XTT> {
  let PrintMethod = "printXAIcoreTileMemory";
}

//===----------------------------------------------------------------------===//
// 指令基类
//===----------------------------------------------------------------------===//

/// 定长 32-bit 的 YAI 指令基类。
///
/// base 携带所有固定位（major/sel/vec/funct/field），子类只覆盖寄存器
/// 字段。hasNoSchedulingInfo 让这一阶段不依赖任何 itinerary；
/// hasSideEffects 防止配置读写和 tile 访存被 DCE 或自由重排。
class XAIcoreInst<dag outs, dag ins, string opcodestr, string argstr,
                  bits<32> base>
    : RVInstCommon<outs, ins, opcodestr, argstr, [], InstFormatOther> {
  field bits<32> Inst;
  field bits<32> SoftFail = 0;

  let Size = 4;
  let Inst = base;
  let Predicates = [HasVendorXAIcore];
  let hasSideEffects = 1;
  let mayLoad = 0;
  let mayStore = 0;
  let hasNoSchedulingInfo = 1;
}

//===----------------------------------------------------------------------===//
// Tile 值定义：tile_create
//
// 固定 t0 阶段的“句柄”已经消失：tile_create 定义一个新的 XTT 值，RA 负责
// 决定它落在 t0..t15 的哪一个。伪指令在 post-RA 展开成 tcopy $d, $d，保留
// 物理寄存器定义（不引入“未定义物理寄存器”）。
//===----------------------------------------------------------------------===//

def XAIcoreTCreate : Pseudo<(outs XTT:$d), (ins), [], "", ""> {
  let Predicates = [HasVendorXAIcore];
  let hasSideEffects = 1;
  let mayLoad = 0;
  let mayStore = 0;
}

//===----------------------------------------------------------------------===//
// cfg.setx $d, <field>, <gpr>
//===----------------------------------------------------------------------===//

def XAIcoreCfgSetxTCDtype
    : XAIcoreInst<(outs), (ins XTTCOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, dtype, $rs1", 0x70000002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxTCRows
    : XAIcoreInst<(outs), (ins XTTCOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, rows, $rs1", 0x70020002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxTCCols
    : XAIcoreInst<(outs), (ins XTTCOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, cols, $rs1", 0x70040002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxTMBaseAddr
    : XAIcoreInst<(outs), (ins XTTMOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, base_addr, $rs1", 0x78000002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxTMRowStride
    : XAIcoreInst<(outs), (ins XTTMOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, row_stride_bytes, $rs1", 0x78020002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxTMColStride
    : XAIcoreInst<(outs), (ins XTTMOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, col_stride_bytes, $rs1", 0x78040002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxTMTransform
    : XAIcoreInst<(outs), (ins XTTMOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, transform, $rs1", 0x78060002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

//===----------------------------------------------------------------------===//
// cfg.get <gpr>, $d, <field>
//
// cfg.get 与 cfg.setx 的寄存器字段方向相反：配置寄存器编号在 bit 11-7
// （rs1 位域），目的 GPR 在 bit 6-2（rd 位域）。
//===----------------------------------------------------------------------===//

def XAIcoreCfgGetTCRows
    : XAIcoreInst<(outs GPR:$rd), (ins XTTCOp:$d), "cfg.get",
                  "$rd, $d, rows", 0x70820002> {
  bits<5> rd;
  bits<5> d;
  let Inst{6-2} = rd;
  let Inst{11-7} = d;
}

def XAIcoreCfgGetTCCols
    : XAIcoreInst<(outs GPR:$rd), (ins XTTCOp:$d), "cfg.get",
                  "$rd, $d, cols", 0x70840002> {
  bits<5> rd;
  bits<5> d;
  let Inst{6-2} = rd;
  let Inst{11-7} = d;
}

//===----------------------------------------------------------------------===//
// tload / tstore / tcopy
//
// tload/tstore 访问的是“这个 Tile 寄存器”，所以 Tile 操作数是一个 use
// （寄存器身份不变，数据由硬件更新）。tcopy 同时定义目的、使用源。
//===----------------------------------------------------------------------===//

def XAIcoreTLoad
    : XAIcoreInst<(outs), (ins XTTOp:$d), "tload", "$d", 0x00000001> {
  bits<5> d;
  let Inst{6-2} = d;
  let mayLoad = 1;
}

def XAIcoreTStore
    : XAIcoreInst<(outs), (ins XTTOp:$d), "tstore", "$d", 0x00400001> {
  bits<5> d;
  let Inst{6-2} = d;
  let mayStore = 1;
}

def XAIcoreTCopy
    : XAIcoreInst<(outs XTTOp:$d), (ins XTTOp:$s), "tcopy", "$d, $s",
                  0x20800001> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

//===----------------------------------------------------------------------===//
// Acc 数据寄存器（XTA0..XTA15）
//
// 一个 Acc 是 32x32 个 i32 = 32768 bit。模拟器与编译器统一为 **16** 个 Acc
// （state.h: ACC_REGISTERS = 16）。
//===----------------------------------------------------------------------===//

def XTA0 : RISCVReg<0, "xta0">;
def XTA1 : RISCVReg<1, "xta1">;
def XTA2 : RISCVReg<2, "xta2">;
def XTA3 : RISCVReg<3, "xta3">;
def XTA4 : RISCVReg<4, "xta4">;
def XTA5 : RISCVReg<5, "xta5">;
def XTA6 : RISCVReg<6, "xta6">;
def XTA7 : RISCVReg<7, "xta7">;
def XTA8 : RISCVReg<8, "xta8">;
def XTA9 : RISCVReg<9, "xta9">;
def XTA10 : RISCVReg<10, "xta10">;
def XTA11 : RISCVReg<11, "xta11">;
def XTA12 : RISCVReg<12, "xta12">;
def XTA13 : RISCVReg<13, "xta13">;
def XTA14 : RISCVReg<14, "xta14">;
def XTA15 : RISCVReg<15, "xta15">;

def XTA : RegisterClass<"RISCV", [v1024i32], 32768,
                        (sequence "XTA%u", 0, 15)> {
  let Size = 32768;
}

def XTAOp : RegisterOperand<XTA> {
  let PrintMethod = "printXAIcoreAccReg";
}
def XTACOp : RegisterOperand<XTA> {
  let PrintMethod = "printXAIcoreAccConfig";
}
def XTAMOp : RegisterOperand<XTA> {
  let PrintMethod = "printXAIcoreAccMemory";
}

//===----------------------------------------------------------------------===//
// Vec32 数据寄存器（XTV0..XTV31，32 x i32 = 1024 bit）
// Vec8  数据寄存器（XTB0..XTB31，32 x i8  =  256 bit）
//===----------------------------------------------------------------------===//

def XTV0 : RISCVReg<0, "xtv0">;
def XTV1 : RISCVReg<1, "xtv1">;
def XTV2 : RISCVReg<2, "xtv2">;
def XTV3 : RISCVReg<3, "xtv3">;
def XTV4 : RISCVReg<4, "xtv4">;
def XTV5 : RISCVReg<5, "xtv5">;
def XTV6 : RISCVReg<6, "xtv6">;
def XTV7 : RISCVReg<7, "xtv7">;
def XTV8 : RISCVReg<8, "xtv8">;
def XTV9 : RISCVReg<9, "xtv9">;
def XTV10 : RISCVReg<10, "xtv10">;
def XTV11 : RISCVReg<11, "xtv11">;
def XTV12 : RISCVReg<12, "xtv12">;
def XTV13 : RISCVReg<13, "xtv13">;
def XTV14 : RISCVReg<14, "xtv14">;
def XTV15 : RISCVReg<15, "xtv15">;
def XTV16 : RISCVReg<16, "xtv16">;
def XTV17 : RISCVReg<17, "xtv17">;
def XTV18 : RISCVReg<18, "xtv18">;
def XTV19 : RISCVReg<19, "xtv19">;
def XTV20 : RISCVReg<20, "xtv20">;
def XTV21 : RISCVReg<21, "xtv21">;
def XTV22 : RISCVReg<22, "xtv22">;
def XTV23 : RISCVReg<23, "xtv23">;
def XTV24 : RISCVReg<24, "xtv24">;
def XTV25 : RISCVReg<25, "xtv25">;
def XTV26 : RISCVReg<26, "xtv26">;
def XTV27 : RISCVReg<27, "xtv27">;
def XTV28 : RISCVReg<28, "xtv28">;
def XTV29 : RISCVReg<29, "xtv29">;
def XTV30 : RISCVReg<30, "xtv30">;
def XTV31 : RISCVReg<31, "xtv31">;

def XTV : RegisterClass<"RISCV", [v32i32], 1024,
                        (sequence "XTV%u", 0, 31)> {
  let Size = 1024;
}

def XTVOp : RegisterOperand<XTV> {
  let PrintMethod = "printXAIcoreVec32Reg";
}
def XTVCOp : RegisterOperand<XTV> {
  let PrintMethod = "printXAIcoreVec32Config";
}
def XTVMOp : RegisterOperand<XTV> {
  let PrintMethod = "printXAIcoreVec32Memory";
}

def XTB0 : RISCVReg<0, "xtb0">;
def XTB1 : RISCVReg<1, "xtb1">;
def XTB2 : RISCVReg<2, "xtb2">;
def XTB3 : RISCVReg<3, "xtb3">;
def XTB4 : RISCVReg<4, "xtb4">;
def XTB5 : RISCVReg<5, "xtb5">;
def XTB6 : RISCVReg<6, "xtb6">;
def XTB7 : RISCVReg<7, "xtb7">;
def XTB8 : RISCVReg<8, "xtb8">;
def XTB9 : RISCVReg<9, "xtb9">;
def XTB10 : RISCVReg<10, "xtb10">;
def XTB11 : RISCVReg<11, "xtb11">;
def XTB12 : RISCVReg<12, "xtb12">;
def XTB13 : RISCVReg<13, "xtb13">;
def XTB14 : RISCVReg<14, "xtb14">;
def XTB15 : RISCVReg<15, "xtb15">;
def XTB16 : RISCVReg<16, "xtb16">;
def XTB17 : RISCVReg<17, "xtb17">;
def XTB18 : RISCVReg<18, "xtb18">;
def XTB19 : RISCVReg<19, "xtb19">;
def XTB20 : RISCVReg<20, "xtb20">;
def XTB21 : RISCVReg<21, "xtb21">;
def XTB22 : RISCVReg<22, "xtb22">;
def XTB23 : RISCVReg<23, "xtb23">;
def XTB24 : RISCVReg<24, "xtb24">;
def XTB25 : RISCVReg<25, "xtb25">;
def XTB26 : RISCVReg<26, "xtb26">;
def XTB27 : RISCVReg<27, "xtb27">;
def XTB28 : RISCVReg<28, "xtb28">;
def XTB29 : RISCVReg<29, "xtb29">;
def XTB30 : RISCVReg<30, "xtb30">;
def XTB31 : RISCVReg<31, "xtb31">;

def XTB : RegisterClass<"RISCV", [v32i8], 256,
                        (sequence "XTB%u", 0, 31)> {
  let Size = 256;
}

def XTBOp : RegisterOperand<XTB> {
  let PrintMethod = "printXAIcoreVec8Reg";
}
def XTBCOp : RegisterOperand<XTB> {
  let PrintMethod = "printXAIcoreVec8Config";
}
def XTBMOp : RegisterOperand<XTB> {
  let PrintMethod = "printXAIcoreVec8Memory";
}

//===----------------------------------------------------------------------===//
// Acc / Vec 值的定义（create）
//
// 与 tile_create 相同：伪指令定义一个新的寄存器值，post-RA 展开成自拷贝
// （acopy/vcopy/bcopy），保留物理寄存器定义。
//===----------------------------------------------------------------------===//

def XAIcoreACreate : Pseudo<(outs XTA:$d), (ins), [], "", ""> {
  let Predicates = [HasVendorXAIcore];
  let hasSideEffects = 1;
  let mayLoad = 0;
  let mayStore = 0;
}

def XAIcoreVec32Create : Pseudo<(outs XTV:$d), (ins), [], "", ""> {
  let Predicates = [HasVendorXAIcore];
  let hasSideEffects = 1;
  let mayLoad = 0;
  let mayStore = 0;
}

def XAIcoreVec8Create : Pseudo<(outs XTB:$d), (ins), [], "", ""> {
  let Predicates = [HasVendorXAIcore];
  let hasSideEffects = 1;
  let mayLoad = 0;
  let mayStore = 0;
}

//===----------------------------------------------------------------------===//
// Acc 配置：cfg.setx acN / amN, <field>, <gpr>
//===----------------------------------------------------------------------===//

def XAIcoreCfgSetxACDtype
    : XAIcoreInst<(outs), (ins XTACOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, dtype, $rs1", 0x74000002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxACRows
    : XAIcoreInst<(outs), (ins XTACOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, rows, $rs1", 0x74020002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxACCols
    : XAIcoreInst<(outs), (ins XTACOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, cols, $rs1", 0x74040002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxAMBaseAddr
    : XAIcoreInst<(outs), (ins XTAMOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, base_addr, $rs1", 0x7C000002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxAMRowStride
    : XAIcoreInst<(outs), (ins XTAMOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, row_stride_bytes, $rs1", 0x7C020002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxAMColStride
    : XAIcoreInst<(outs), (ins XTAMOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, col_stride_bytes, $rs1", 0x7C040002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

//===----------------------------------------------------------------------===//
// Acc load/store 与拷贝
//===----------------------------------------------------------------------===//

def XAIcoreALoad
    : XAIcoreInst<(outs), (ins XTAOp:$d), "aload", "$d", 0x04000001> {
  bits<5> d;
  let Inst{6-2} = d;
  let mayLoad = 1;
}

def XAIcoreAStore
    : XAIcoreInst<(outs), (ins XTAOp:$d), "astore", "$d", 0x04400001> {
  bits<5> d;
  let Inst{6-2} = d;
  let mayStore = 1;
}

def XAIcoreACopy
    : XAIcoreInst<(outs XTAOp:$d), (ins XTAOp:$s), "acopy", "$d, $s",
                  0x24800001> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

def XAIcoreVLoad
    : XAIcoreInst<(outs), (ins XTVOp:$d), "vload", "$d", 0x0C000001> {
  bits<5> d;
  let Inst{6-2} = d;
  let mayLoad = 1;
}

def XAIcoreVStore
    : XAIcoreInst<(outs), (ins XTVOp:$d), "vstore", "$d", 0x0C400001> {
  bits<5> d;
  let Inst{6-2} = d;
  let mayStore = 1;
}

def XAIcoreVCopy
    : XAIcoreInst<(outs XTVOp:$d), (ins XTVOp:$s), "vcopy", "$d, $s",
                  0x2C800001> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

def XAIcoreBLoad
    : XAIcoreInst<(outs), (ins XTBOp:$d), "bload", "$d", 0x08000001> {
  bits<5> d;
  let Inst{6-2} = d;
  let mayLoad = 1;
}

def XAIcoreBStore
    : XAIcoreInst<(outs), (ins XTBOp:$d), "bstore", "$d", 0x08400001> {
  bits<5> d;
  let Inst{6-2} = d;
  let mayStore = 1;
}

def XAIcoreBCopy
    : XAIcoreInst<(outs XTBOp:$d), (ins XTBOp:$s), "bcopy", "$d, $s",
                  0x28800001> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

//===----------------------------------------------------------------------===//
// Vec32 配置：cfg.setx vcN, {dtype,len}, rs1 / vmN, {base_addr,stride_bytes}, rs1
//
// cfg.setx 的 vec 位（bit 25）= 1，sel：vc=1，vm=3。
//===----------------------------------------------------------------------===//

def XAIcoreCfgSetxVCDtype
    : XAIcoreInst<(outs), (ins XTVCOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, dtype, $rs1", 0x76000002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxVCLen
    : XAIcoreInst<(outs), (ins XTVCOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, len, $rs1", 0x76020002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxVMBaseAddr
    : XAIcoreInst<(outs), (ins XTVMOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, base_addr, $rs1", 0x7E000002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

def XAIcoreCfgSetxVMStride
    : XAIcoreInst<(outs), (ins XTVMOp:$d, GPR:$rs1), "cfg.setx",
                  "$d, stride_bytes, $rs1", 0x7E020002> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}
//===----------------------------------------------------------------------===//
// Vec32 运算（E_BIN / E_UN / INIT / 归约 / 广播）
//
// 与 Acc 一样，目标寄存器建模成 use：硬件原地写，SSA 身份不变。
//===----------------------------------------------------------------------===//

// vec32 标量运算：E_BIN，dom2=11 (vec32)、BX=1（src2 是标量 xS），funct4 同 E_BIN。
class XAIcoreVBinX<bits<32> base, string mnemonic>
    : XAIcoreInst<(outs), (ins XTVOp:$d, XTVOp:$s, GPR:$rs1), mnemonic,
                  "$d, $s, $rs1", base> {
  bits<5> d;
  bits<5> s;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
  let Inst{16-12} = rs1;
}

def XAIcoreVAddX : XAIcoreVBinX<0x0E000000, "vaddx">;
def XAIcoreVMulX : XAIcoreVBinX<0x0E400000, "vmulx">;

// vec32 二元：E_BIN，dom2=11 (vec32)、BX=0（第二个源也是 vec32）。
class XAIcoreVBin<bits<32> base, string mnemonic>
    : XAIcoreInst<(outs), (ins XTVOp:$d, XTVOp:$s1, XTVOp:$s2), mnemonic,
                  "$d, $s1, $s2", base> {
  bits<5> d;
  bits<5> s1;
  bits<5> s2;
  let Inst{6-2} = d;
  let Inst{11-7} = s1;
  let Inst{16-12} = s2;
}

def XAIcoreVAdd : XAIcoreVBin<0x0C000000, "vadd">;
def XAIcoreVSub : XAIcoreVBin<0x0C200000, "vsub">;
def XAIcoreVMul : XAIcoreVBin<0x0C400000, "vmul">;
def XAIcoreVDiv : XAIcoreVBin<0x0C600000, "vdiv">;
def XAIcoreVMax : XAIcoreVBin<0x0CA00000, "vmax">;

// cfg.copy vcD, vcS：整寄存器配置克隆（R4，funct3 = 001）。
def XAIcoreCfgCopyVC
    : XAIcoreInst<(outs), (ins XTVCOp:$d, XTVCOp:$s), "cfg.copy", "$d, $s",
                  0x76400002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

// cfg.copy 的其余 7 个域（寄存器分配器插入的拷贝必须连描述符一起复制，否则
// 目标寄存器的 dtype/len/base/strides 是陈旧值 —— 数据对了、元数据错了）。
// 编码 = 0x70000002 | (reg_type << 25) | (1 << 22)；reg_type 见模拟器的
// cfg_reg_type(sel2, vector_side)：tc=0 bc=1 ac=2 vc=3 tm=4 bm=5 am=6 vm=7。
def XAIcoreCfgCopyTC
    : XAIcoreInst<(outs), (ins XTTCOp:$d, XTTCOp:$s), "cfg.copy", "$d, $s",
                  0x70400002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}
def XAIcoreCfgCopyAC
    : XAIcoreInst<(outs), (ins XTACOp:$d, XTACOp:$s), "cfg.copy", "$d, $s",
                  0x74400002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}
def XAIcoreCfgCopyBC
    : XAIcoreInst<(outs), (ins XTBCOp:$d, XTBCOp:$s), "cfg.copy", "$d, $s",
                  0x72400002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}
def XAIcoreCfgCopyTM
    : XAIcoreInst<(outs), (ins XTTMOp:$d, XTTMOp:$s), "cfg.copy", "$d, $s",
                  0x78400002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}
def XAIcoreCfgCopyAM
    : XAIcoreInst<(outs), (ins XTAMOp:$d, XTAMOp:$s), "cfg.copy", "$d, $s",
                  0x7C400002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}
def XAIcoreCfgCopyBM
    : XAIcoreInst<(outs), (ins XTBMOp:$d, XTBMOp:$s), "cfg.copy", "$d, $s",
                  0x7A400002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}
def XAIcoreCfgCopyVM
    : XAIcoreInst<(outs), (ins XTVMOp:$d, XTVMOp:$s), "cfg.copy", "$d, $s",
                  0x7E400002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

// vec32 一元：E_UN，rsqrt.approx（funct3 = 5）。
def XAIcoreVRsqrt
    : XAIcoreInst<(outs), (ins XTVOp:$d, XTVOp:$s), "vrsqrt.approx",
                  "$d, $s", 0x1D400000> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

// vec32 fillx：INIT_MOVE，dest 是 vec32，src 是标量 xS。
def XAIcoreVFillX
    : XAIcoreInst<(outs), (ins XTVOp:$d, GPR:$rs1), "vfillx", "$d, $rs1",
                  0x2C400001> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

// Acc 行归约：每行平方和，累加到 vec32（v += sumsq(a)）。
//   [31:28]=0001 [24:22]=funct(sum=0,max=1,min=2,sumsq=3,argmax=4,absmax=5)
//   [11:7]=src(acc) [6:2]=dest(vec) [1:0]=10
def XAIcoreAccRowSumSq
    : XAIcoreInst<(outs), (ins XTVOp:$d, XTAOp:$s), "areduce.rows.sumsq",
                  "$d, $s", 0x10C00002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

def XAIcoreAccRowSum
    : XAIcoreInst<(outs), (ins XTVOp:$d, XTAOp:$s), "areduce.rows.sum",
                  "$d, $s", 0x10000002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

def XAIcoreAccRowMax
    : XAIcoreInst<(outs), (ins XTVOp:$d, XTAOp:$s), "areduce.rows.max",
                  "$d, $s", 0x10400002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

def XAIcoreAccRowAbsMax
    : XAIcoreInst<(outs), (ins XTVOp:$d, XTAOp:$s), "areduce.rows.absmax",
                  "$d, $s", 0x11400002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

// areduce.rows.argmax：dst 是 i32 向量（vc.dtype=i32、len=rows）。
def XAIcoreAccRowArgMax
    : XAIcoreInst<(outs), (ins XTVOp:$d, XTAOp:$s), "areduce.rows.argmax",
                  "$d, $s", 0x11000002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

// cfg.get rd, acN, rows：归约出来的新 Vec32 需要 rows 作为 vc.len。
def XAIcoreCfgGetACRows
    : XAIcoreInst<(outs GPR:$rd), (ins XTACOp:$d), "cfg.get",
                  "$rd, $d, rows", 0x74820002> {
  bits<5> rd;
  bits<5> d;
  let Inst{6-2} = rd;
  let Inst{11-7} = d;
}

// amaskx.tril aD, aS, xD, fill5：fill5 是 5 bit 立即数（码表见 ISA）。
def XAIcoreImm5 : Operand<i32>;

def XAIcoreAMaskTril
    : XAIcoreInst<(outs), (ins XTAOp:$d, XTAOp:$s, GPR:$xd, XAIcoreImm5:$fill),
                  "amaskx.tril", "$d, $s, $xd, $fill", 0x44400000> {
  bits<5> d;
  bits<5> s;
  bits<5> xd;
  bits<5> fill;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
  let Inst{16-12} = xd;
  let Inst{21-17} = fill;
}

// Acc × Vec32 广播：E_BCAST，byrow 用 S[row]，bycol 用 S[col]。
class XAIcoreAccVecBcast<bits<32> base, string mnemonic>
    : XAIcoreInst<(outs), (ins XTAOp:$d, XTAOp:$s, XTVOp:$v), mnemonic,
                  "$d, $s, $v", base> {
  bits<5> d;
  bits<5> s;
  bits<5> v;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
  let Inst{16-12} = v;
}

def XAIcoreAccAddVecByRow : XAIcoreAccVecBcast<0x58000000, "aaddv.byrow">;
def XAIcoreAccAddVecByCol : XAIcoreAccVecBcast<0x5A000000, "aaddv.bycol">;
def XAIcoreAccSubVecByRow : XAIcoreAccVecBcast<0x58200000, "asubv.byrow">;
def XAIcoreAccSubVecByCol : XAIcoreAccVecBcast<0x5A200000, "asubv.bycol">;
def XAIcoreAccMulVecByRow : XAIcoreAccVecBcast<0x58400000, "amulv.byrow">;
def XAIcoreAccMulVecByCol : XAIcoreAccVecBcast<0x5A400000, "amulv.bycol">;
def XAIcoreAccDivVecByRow : XAIcoreAccVecBcast<0x58600000, "adivv.byrow">;
def XAIcoreAccDivVecByCol : XAIcoreAccVecBcast<0x5A600000, "adivv.bycol">;

//===----------------------------------------------------------------------===//
// 行 gather（embedding）：tgather tD, vIndex
//
// M_BLK（R2）：[31:28]=0000 [27:26]=dom2(0=tile) [25]=0 [24:22]=funct3(010)
//   [11:7]=vIndex（Vec32，i32） [6:2]=dest(tile) [1:0]=01
//
// 表地址/跨步由 dest tile 的 tm 描述符给出：dst[i][j] = table[index[i]][j]，
// 超出 index.len 的行清零。
//===----------------------------------------------------------------------===//

// vgather：vec32 的 lane gather（表地址/跨步来自 dest 的 vm），
//   dst[i] = table[index[i]]
def XAIcoreVGather
    : XAIcoreInst<(outs), (ins XTVOp:$d, XTVOp:$index), "vgather",
                  "$d, $index", 0x0C800001> {
  bits<5> d;
  bits<5> index;
  let Inst{6-2} = d;
  let Inst{11-7} = index;
}

def XAIcoreTGather
    : XAIcoreInst<(outs), (ins XTTOp:$d, XTVOp:$v), "tgather", "$d, $v",
                  0x00800001> {
  bits<5> d;
  bits<5> v;
  let Inst{6-2} = d;
  let Inst{11-7} = v;
}

//===----------------------------------------------------------------------===//
// 量化：tquant.rows.q8s32 tD, vScale, aS
//
// QNT（R4）：[31:28]=0100 [27:26]=dom2(0=tile) [25]=IS [24:22]=funct3
//   [21:17]=vScale [16:12]=index [11:7]=src(acc) [6:2]=dest(tile) [1:0]=10
//
// 动态形态（funct3=0）：每行 scale = absmax/127（写回 vScale），
// q = RNE(v/scale) 饱和到 [-127,127]，NaN→0。
// 目标 tile 的 tc（dtype=i8、rows/cols 与 acc 相同）和 vScale 的 vc
// （dtype=f32、len=rows）必须由用户先配置好。
//===----------------------------------------------------------------------===//

def XAIcoreTQuantX
    : XAIcoreInst<(outs), (ins XTTOp:$d, XTVOp:$v, XTAOp:$s),
                  "tquantx.rows.q8s32", "$d, $v, $s", 0x40400002> {
  bits<5> d;
  bits<5> v;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
  let Inst{21-17} = v;
}

def XAIcoreTQuant
    : XAIcoreInst<(outs), (ins XTTOp:$d, XTVOp:$v, XTAOp:$s),
                  "tquant.rows.q8s32", "$d, $v, $s", 0x40000002> {
  bits<5> d;
  bits<5> v;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
  let Inst{21-17} = v;
}

//===----------------------------------------------------------------------===//
// Acc 标量运算 / fill / 一元
//
//   aaddx/amulx aD, aS, xS   ：E_BIN，acc 域 BX=1（funct4 add=0/mul=2）
//   afillx aD, xS            ：INIT_MOVE fillx（domain acc）
//   aexp2.approx/asilu.approx/aabs aD, aS ：E_UNARY（funct3 exp2=3/silu=6/abs=1）
//   vexp2.approx vD, vS      ：E_UNARY（vec32）
//===----------------------------------------------------------------------===//

class XAIcoreABinX<bits<32> base, string mnemonic>
    : XAIcoreInst<(outs), (ins XTAOp:$d, XTAOp:$s, GPR:$rs1), mnemonic,
                  "$d, $s, $rs1", base> {
  bits<5> d;
  bits<5> s;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
  let Inst{16-12} = rs1;
}

def XAIcoreAAddX : XAIcoreABinX<0x06000000, "aaddx">;
def XAIcoreAMulX : XAIcoreABinX<0x06400000, "amulx">;

def XAIcoreAFillX
    : XAIcoreInst<(outs), (ins XTAOp:$d, GPR:$rs1), "afillx", "$d, $rs1",
                  0x24400001> {
  bits<5> d;
  bits<5> rs1;
  let Inst{6-2} = d;
  let Inst{11-7} = rs1;
}

class XAIcoreAUn<bits<32> base, string mnemonic>
    : XAIcoreInst<(outs), (ins XTAOp:$d, XTAOp:$s), mnemonic, "$d, $s", base> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

def XAIcoreAAbs : XAIcoreAUn<0x14400000, "aabs">;
def XAIcoreAExp2 : XAIcoreAUn<0x14C00000, "aexp2.approx">;
def XAIcoreASilu : XAIcoreAUn<0x15800000, "asilu.approx">;

class XAIcoreVUn<bits<32> base, string mnemonic>
    : XAIcoreInst<(outs), (ins XTVOp:$d, XTVOp:$s), mnemonic, "$d, $s", base> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

def XAIcoreVExp2 : XAIcoreVUn<0x1CC00000, "vexp2.approx">;

//===----------------------------------------------------------------------===//
// 矩阵乘：mma.nn.{zero,acc}.i8.i32 <acc>, <tileA>, <tileB>
//
// 只支持非转置（nn）形式；转置（nt/tn/tt）留到后续。
// 编码：accbit<<22 | B<<12 | A<<7 | acc<<2 | 2
//===----------------------------------------------------------------------===//

class XAIcoreMMA<bits<32> base, string mnemonic>
    : XAIcoreInst<(outs), (ins XTAOp:$acc, XTTOp:$a, XTTOp:$b), mnemonic,
                  "$acc, $a, $b", base> {
  bits<5> acc;
  bits<5> a;
  bits<5> b;
  let Inst{6-2} = acc;
  let Inst{11-7} = a;
  let Inst{16-12} = b;
}

def XAIcoreMMANNZero : XAIcoreMMA<0x00000002, "mma.nn.zero.i8.i32">;
def XAIcoreMMANNAcc : XAIcoreMMA<0x00400002, "mma.nn.acc.i8.i32">;
// 右侧转置（attention 的 QK^T）：funct3 = 010 / 011。
def XAIcoreMMANTZero : XAIcoreMMA<0x00800002, "mma.nt.zero.i8.i32">;
def XAIcoreMMANTAcc : XAIcoreMMA<0x00C00002, "mma.nt.acc.i8.i32">;

// twiden.i8.i32 aD, tS：Tile i8 -> Acc i32 符号扩展（目标 shape 取源 tile）。
def XAIcoreTWiden
    : XAIcoreInst<(outs), (ins XTAOp:$d, XTTOp:$s), "twiden.i8.i32",
                  "$d, $s", 0x34400002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

// acvt.i32.f32 aD, aS：Acc int32 -> f32（原地时配置 dtype 由硬件改成 f32）。
def XAIcoreACvt
    : XAIcoreInst<(outs), (ins XTAOp:$d, XTAOp:$s), "acvt.i32.f32",
                  "$d, $s", 0x34000002> {
  bits<5> d;
  bits<5> s;
  let Inst{6-2} = d;
  let Inst{11-7} = s;
}

//===----------------------------------------------------------------------===//
// Acc 逐元素二元运算：aadd/asub/amul/adiv <d>, <s1>, <s2>
//
// E_BIN（R3）格式：
//   [31:28] major4 = 0000  [27:25] sel3 = {dom2, BX}（acc: dom2=01, BX=0 → 010）
//   [24:21] funct4 = add 0 / sub 1 / mul 2 / div 3
//   [16:12] src2  [11:7] src1  [6:2] dest  [1:0] q = 00
//
// C++ API 是原地运算（a.add(b)），所以 dest 与 src1 是同一个寄存器；三个
// 操作数都建模成 use（硬件原地写，SSA 身份不变）。
// div 只对 f32 合法（i32 的 div 由硬件/模拟器在运行时拒绝）。
//===----------------------------------------------------------------------===//

class XAIcoreEbin<bits<32> base, string mnemonic>
    : XAIcoreInst<(outs), (ins XTAOp:$d, XTAOp:$s1, XTAOp:$s2), mnemonic,
                  "$d, $s1, $s2", base> {
  bits<5> d;
  bits<5> s1;
  bits<5> s2;
  let Inst{6-2} = d;
  let Inst{11-7} = s1;
  let Inst{16-12} = s2;
}

def XAIcoreAAdd : XAIcoreEbin<0x04000000, "aadd">;
def XAIcoreASub : XAIcoreEbin<0x04200000, "asub">;
def XAIcoreAMul : XAIcoreEbin<0x04400000, "amul">;
def XAIcoreADiv : XAIcoreEbin<0x04600000, "adiv">;
