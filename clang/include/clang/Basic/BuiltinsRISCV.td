//==- BuiltinsRISCV.td - RISC-V Builtin function database ---*- tablegen -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the RISC-V-specific builtin function database.
//
//===----------------------------------------------------------------------===//

include "clang/Basic/BuiltinsBase.td"

class RISCVBuiltin<string prototype, string features = ""> : TargetBuiltin {
  let Spellings = ["__builtin_riscv_" # NAME];
  let Prototype = prototype;
  let Features = features;
}

let Attributes = [NoThrow, Const] in {
//===----------------------------------------------------------------------===//
// Zbb extension.
//===----------------------------------------------------------------------===//
def orc_b_32 : RISCVBuiltin<"unsigned int(unsigned int)", "zbb">;
def orc_b_64 : RISCVBuiltin<"uint64_t(uint64_t)", "zbb,64bit">;
def clz_32 : RISCVBuiltin<"unsigned int(unsigned int)", "zbb|xtheadbb">;
def clz_64 : RISCVBuiltin<"unsigned int(uint64_t)", "zbb|xtheadbb,64bit">;
def ctz_32 : RISCVBuiltin<"unsigned int(unsigned int)", "zbb">;
def ctz_64 : RISCVBuiltin<"unsigned int(uint64_t)", "zbb,64bit">;

//===----------------------------------------------------------------------===//
// Zbc or Zbkc extension.
//===----------------------------------------------------------------------===//
def clmul_32 : RISCVBuiltin<"unsigned int(unsigned int, unsigned int)", "zbc|zbkc">;
def clmul_64 : RISCVBuiltin<"uint64_t(uint64_t, uint64_t)", "zbc|zbkc,64bit">;
def clmulh_32 : RISCVBuiltin<"unsigned int(unsigned int, unsigned int)", "zbc|zbkc,32bit">;
def clmulh_64 : RISCVBuiltin<"uint64_t(uint64_t, uint64_t)", "zbc|zbkc,64bit">;
def clmulr_32 : RISCVBuiltin<"unsigned int(unsigned int, unsigned int)", "zbc,32bit">;
def clmulr_64 : RISCVBuiltin<"uint64_t(uint64_t, uint64_t)", "zbc,64bit">;

//===----------------------------------------------------------------------===//
// Zbkx extension.
//===----------------------------------------------------------------------===//
let Features = "zbkx,32bit" in {
def xperm4_32 : RISCVBuiltin<"unsigned int(unsigned int, unsigned int)">;
def xperm8_32 : RISCVBuiltin<"unsigned int(unsigned int, unsigned int)">;
} // Features = "zbkx,32bit"

let Features = "zbkx,64bit" in {
def xperm4_64 : RISCVBuiltin<"uint64_t(uint64_t, uint64_t)">;
def xperm8_64 : RISCVBuiltin<"uint64_t(uint64_t, uint64_t)">;
} // Features = "zbkx,64bit"

//===----------------------------------------------------------------------===//
// Zbkb extension.
//===----------------------------------------------------------------------===//
def brev8_32 : RISCVBuiltin<"unsigned int(unsigned int)", "zbkb">;
def brev8_64 : RISCVBuiltin<"uint64_t(uint64_t)", "zbkb,64bit">;
def zip_32 : RISCVBuiltin<"unsigned int(unsigned int)", "zbkb,32bit">;
def unzip_32 : RISCVBuiltin<"unsigned int(unsigned int)", "zbkb,32bit">;

//===----------------------------------------------------------------------===//
// Zknd extension.
//===----------------------------------------------------------------------===//
let Features = "zknd,32bit" in {
def aes32dsi : RISCVBuiltin<"unsigned int(unsigned int, unsigned int, _Constant unsigned int)">;
def aes32dsmi : RISCVBuiltin<"unsigned int(unsigned int, unsigned int, _Constant unsigned int)">;
} // Features = "zknd,32bit"

let Features = "zknd,64bit" in {
def aes64ds : RISCVBuiltin<"uint64_t(uint64_t, uint64_t)">;
def aes64dsm : RISCVBuiltin<"uint64_t(uint64_t, uint64_t)">;
def aes64im : RISCVBuiltin<"uint64_t(uint64_t)">;
} // Features = "zknd,64bit"

//===----------------------------------------------------------------------===//
// Zknd & Zkne extension.
//===----------------------------------------------------------------------===//
let Features = "zknd|zkne,64bit" in {
def aes64ks1i : RISCVBuiltin<"uint64_t(uint64_t, _Constant unsigned int)">;
def aes64ks2 : RISCVBuiltin<"uint64_t(uint64_t, uint64_t)">;
} // Features = "zknd|zkne,64bit"

//===----------------------------------------------------------------------===//
// Zkne extension.
//===----------------------------------------------------------------------===//
let Features = "zkne,32bit" in {
def aes32esi : RISCVBuiltin<"unsigned int(unsigned int, unsigned int, _Constant unsigned int)">;
def aes32esmi : RISCVBuiltin<"unsigned int(unsigned int, unsigned int, _Constant unsigned int)">;
} // Features = "zkne,32bit"

let Features = "zkne,64bit" in {
def aes64es : RISCVBuiltin<"uint64_t(uint64_t, uint64_t)">;
def aes64esm : RISCVBuiltin<"uint64_t(uint64_t, uint64_t)">;
} // Features = "zkne,64bit"

//===----------------------------------------------------------------------===//
// Zknh extension.
//===----------------------------------------------------------------------===//
let Features = "zknh" in {
def sha256sig0 : RISCVBuiltin<"unsigned int(unsigned int)">;
def sha256sig1 : RISCVBuiltin<"unsigned int(unsigned int)">;
def sha256sum0 : RISCVBuiltin<"unsigned int(unsigned int)">;
def sha256sum1 : RISCVBuiltin<"unsigned int(unsigned int)">;
} // Features = "zknh"

let Features = "zknh,32bit" in {
def sha512sig0h : RISCVBuiltin<"unsigned int(unsigned int, unsigned int)">;
def sha512sig0l : RISCVBuiltin<"unsigned int(unsigned int, unsigned int)">;
def sha512sig1h : RISCVBuiltin<"unsigned int(unsigned int, unsigned int)">;
def sha512sig1l : RISCVBuiltin<"unsigned int(unsigned int, unsigned int)">;
def sha512sum0r : RISCVBuiltin<"unsigned int(unsigned int, unsigned int)">;
def sha512sum1r : RISCVBuiltin<"unsigned int(unsigned int, unsigned int)">;
} // Features = "zknh,32bit"

let Features = "zknh,64bit" in {
def sha512sig0 : RISCVBuiltin<"uint64_t(uint64_t)">;
def sha512sig1 : RISCVBuiltin<"uint64_t(uint64_t)">;
def sha512sum0 : RISCVBuiltin<"uint64_t(uint64_t)">;
def sha512sum1 : RISCVBuiltin<"uint64_t(uint64_t)">;
} // Features = "zknh,64bit"

//===----------------------------------------------------------------------===//
// Zksed extension.
//===----------------------------------------------------------------------===//
let Features = "zksed" in {
def sm4ed : RISCVBuiltin<"unsigned int(unsigned int, unsigned int, _Constant unsigned int )">;
def sm4ks : RISCVBuiltin<"unsigned int(unsigned int, unsigned int, _Constant unsigned int)">;
} // Features = "zksed"

//===----------------------------------------------------------------------===//
// Zksh extension.
//===----------------------------------------------------------------------===//
let Features = "zksh" in {
def sm3p0 : RISCVBuiltin<"unsigned int(unsigned int)">;
def sm3p1 : RISCVBuiltin<"unsigned int(unsigned int)">;
} // Features = "zksh"

} // Attributes = [Const, NoThrow]

//===----------------------------------------------------------------------===//
// Zihintntl extension.
//===----------------------------------------------------------------------===//
let Features = "zihintntl", Attributes = [CustomTypeChecking] in {
def ntl_load : RISCVBuiltin<"void(...)">;
def ntl_store : RISCVBuiltin<"void(...)">;
} // Features = "zihintntl", Attributes = [CustomTypeChecking]

//===----------------------------------------------------------------------===//
// Zihintpause extension.
//===----------------------------------------------------------------------===//
let Features = "zihintpause", Attributes = [NoThrow] in
def pause : RISCVBuiltin<"void()">;

//===----------------------------------------------------------------------===//
// XCV extensions.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Tile NPU/YAI experimental builtins.
// These are initially lowered to target calls by CodeGen; the RISCV backend
// will later replace those calls with YAI machine instructions.
//===----------------------------------------------------------------------===//
class YAIBuiltin<string prototype> : TargetBuiltin {
  let Spellings = ["__builtin_yai_" # NAME];
  let Prototype = prototype;
  let Attributes = [CustomTypeChecking, NoThrow];
  // Tile/Acc/Vec 值是 xaicore 专用载体，没有扩展时不能编译。
  let Features = "xaicore";
}

def acc_abs : YAIBuiltin<"void(...)">;
def acc_add : YAIBuiltin<"_Vector<1024, int>(...)">;
def acc_add_inplace : YAIBuiltin<"void(...)">;
def acc_add_scalar : YAIBuiltin<"void(...)">;
def acc_add_vec : YAIBuiltin<"void(...)">;
def acc_cos : YAIBuiltin<"void(...)">;
def acc_create : YAIBuiltin<"_Vector<1024, int>(...)">;
def acc_div : YAIBuiltin<"_Vector<1024, int>(...)">;
def acc_div_inplace : YAIBuiltin<"void(...)">;
def acc_div_vec : YAIBuiltin<"void(...)">;
def acc_exp : YAIBuiltin<"void(...)">;
def acc_fill : YAIBuiltin<"void(...)">;
def acc_get_base : YAIBuiltin<"void*(...)">;
def acc_get_data : YAIBuiltin<"void*(...)">;
def acc_get_dtype : YAIBuiltin<"uint64_t(...)">;
def acc_get_stride : YAIBuiltin<"uint64_t(...)">;
def acc_get_valid_zone : YAIBuiltin<"uint64_t(...)">;
def acc_load : YAIBuiltin<"void(...)">;
def acc_mask : YAIBuiltin<"void(...)">;
def acc_mul : YAIBuiltin<"_Vector<1024, int>(...)">;
def acc_mul_inplace : YAIBuiltin<"void(...)">;
def acc_mul_scalar : YAIBuiltin<"void(...)">;
def acc_mul_vec : YAIBuiltin<"void(...)">;
def acc_quant : YAIBuiltin<"void(...)">;
def acc_quant_ext : YAIBuiltin<"void(...)">;
def acc_row_absmax : YAIBuiltin<"_Vector<32, int>(...)">;
def acc_row_absmax_merge : YAIBuiltin<"void(...)">;
def acc_row_argmax : YAIBuiltin<"_Vector<32, int>(...)">;
def acc_row_argmax_merge : YAIBuiltin<"void(...)">;
def acc_row_max : YAIBuiltin<"_Vector<32, int>(...)">;
def acc_row_max_merge : YAIBuiltin<"void(...)">;
def acc_row_sum : YAIBuiltin<"_Vector<32, int>(...)">;
def acc_row_sum_merge : YAIBuiltin<"void(...)">;
def acc_row_sum_sq : YAIBuiltin<"_Vector<32, int>(...)">;
def acc_row_sum_sq_merge : YAIBuiltin<"void(...)">;
def acc_set_base : YAIBuiltin<"void(...)">;
def acc_set_data : YAIBuiltin<"void(...)">;
def acc_set_dtype : YAIBuiltin<"void(...)">;
def acc_set_stride : YAIBuiltin<"void(...)">;
def acc_set_strides : YAIBuiltin<"void(...)">;
def acc_set_valid_zone : YAIBuiltin<"void(...)">;
def acc_set_valid_zone_pair : YAIBuiltin<"void(...)">;
def acc_silu : YAIBuiltin<"void(...)">;
def acc_sin : YAIBuiltin<"void(...)">;
def acc_store : YAIBuiltin<"void(...)">;
def acc_sub : YAIBuiltin<"_Vector<1024, int>(...)">;
def acc_sub_inplace : YAIBuiltin<"void(...)">;
def acc_sub_vec : YAIBuiltin<"void(...)">;
def acc_to_fp32 : YAIBuiltin<"void(...)">;
def tile_create : YAIBuiltin<"_Vector<1024, char>(...)">;
def tile_dequant : YAIBuiltin<"_Vector<1024, int>(...)">;
def tile_fill : YAIBuiltin<"void(...)">;
def tile_gather_from_vec : YAIBuiltin<"void(...)">;
def tile_get_base : YAIBuiltin<"void*(...)">;
def tile_get_data : YAIBuiltin<"void*(...)">;
def tile_get_dtype : YAIBuiltin<"uint64_t(...)">;
def tile_get_stride : YAIBuiltin<"uint64_t(...)">;
def tile_get_transpose : YAIBuiltin<"uint64_t(...)">;
def tile_get_valid_zone : YAIBuiltin<"uint64_t(...)">;
def tile_load : YAIBuiltin<"void(...)">;
def tile_matmul : YAIBuiltin<"void(...)">;
def tile_set_base : YAIBuiltin<"void(...)">;
def tile_set_data : YAIBuiltin<"void(...)">;
def tile_set_dtype : YAIBuiltin<"void(...)">;
def tile_set_stride : YAIBuiltin<"void(...)">;
def tile_set_strides : YAIBuiltin<"void(...)">;
def tile_set_transpose : YAIBuiltin<"void(...)">;
def tile_set_valid_zone : YAIBuiltin<"void(...)">;
def tile_set_valid_zone_pair : YAIBuiltin<"void(...)">;
def tile_store : YAIBuiltin<"void(...)">;
def vec32_add : YAIBuiltin<"void(...)">;
def vec32_add_scalar : YAIBuiltin<"void(...)">;
def vec32_copy : YAIBuiltin<"_Vector<32, int>(...)">;
def tile_widen : YAIBuiltin<"void(...)">;
def vec32_create : YAIBuiltin<"_Vector<32, int>(...)">;
def vec32_div : YAIBuiltin<"void(...)">;
def vec32_exp : YAIBuiltin<"void(...)">;
def vec32_fill : YAIBuiltin<"void(...)">;
def vec32_gather_from_vec : YAIBuiltin<"void(...)">;
def vec32_get_base : YAIBuiltin<"void*(...)">;
def vec32_get_data : YAIBuiltin<"void*(...)">;
def vec32_get_dtype : YAIBuiltin<"uint64_t(...)">;
def vec32_get_stride : YAIBuiltin<"uint64_t(...)">;
def vec32_get_valid_zone : YAIBuiltin<"uint64_t(...)">;
def vec32_load : YAIBuiltin<"void(...)">;
def vec32_max : YAIBuiltin<"void(...)">;
def vec32_mul : YAIBuiltin<"void(...)">;
def vec32_mul_scalar : YAIBuiltin<"void(...)">;
def vec32_rsqrt : YAIBuiltin<"void(...)">;
def vec32_set_base : YAIBuiltin<"void(...)">;
def vec32_set_data : YAIBuiltin<"void(...)">;
def vec32_set_dtype : YAIBuiltin<"void(...)">;
def vec32_set_stride : YAIBuiltin<"void(...)">;
def vec32_set_valid_zone : YAIBuiltin<"void(...)">;
def vec32_store : YAIBuiltin<"void(...)">;
def vec32_sub : YAIBuiltin<"void(...)">;
def vec8_create : YAIBuiltin<"_Vector<32, char>(...)">;
def vec8_gather_from_vec : YAIBuiltin<"void(...)">;
def vec8_get_base : YAIBuiltin<"void*(...)">;
def vec8_get_data : YAIBuiltin<"void*(...)">;
def vec8_get_dtype : YAIBuiltin<"uint64_t(...)">;
def vec8_get_stride : YAIBuiltin<"uint64_t(...)">;
def vec8_get_valid_zone : YAIBuiltin<"uint64_t(...)">;
def vec8_load : YAIBuiltin<"void(...)">;
def vec8_set_base : YAIBuiltin<"void(...)">;
def vec8_set_data : YAIBuiltin<"void(...)">;
def vec8_set_dtype : YAIBuiltin<"void(...)">;
def vec8_set_stride : YAIBuiltin<"void(...)">;
def vec8_set_valid_zone : YAIBuiltin<"void(...)">;
def vec8_store : YAIBuiltin<"void(...)">;
include "clang/Basic/BuiltinsRISCVXCV.td"
