# XAIcore extension (YAI Tile NPU) — vendor branch of LLVM 21.1.7

This branch adds a RISC-V **vendor extension `+xaicore`** that lets C++ code drive a
separate tile-register file (Tile NPU) through `__builtin_yai_*` intrinsics.
It is *not* intended for upstream; it is maintained as a vendor fork of
`llvm/llvm-project` at tag `llvmorg-21.1.7`.

## What the extension provides

| Data domain | LLVM type | Register class | Size |
|---|---|---|---|
| Tile  | `<1024 x i8>`  | `XTT` (16 regs) | 1 KB |
| Acc   | `<1024 x i32>` | `XTA` (16 regs) | 4 KB |
| Vec32 | `<32 x i32>`   | `XTV` (32 regs) | 128 B |
| Vec8  | `<32 x i8>`    | `XTB` (32 regs) | 32 B |

Each physical register additionally carries an out-of-band descriptor
(base/strides/shape/dtype). Those descriptors are **the same physical register
number**, only printed differently (`tN` / `tcN` / `tmN`), so the ordinary register
allocator keeps them consistent as long as the lowering always refers to the same
SSA value for the data and its descriptor. All YAI instructions are
`hasSideEffects = 1`, so they are never reordered, CSE'd or hoisted.

Language surface: `__builtin_yai_<op>(...)` (110 builtins), lowered by clang to a
plain call `__yai_<op>(...)`, which `RISCVTargetLowering::LowerCall` folds into YAI
machine instructions before ABI lowering.

## Changes (35 files: 16 modified, 19 added)

Generic CodeGen:
* `llvm/include/llvm/CodeGen/TargetRegisterInfo.h` — new hook `isSpillableForVirtReg`
* `llvm/lib/CodeGen/RegAllocGreedy.cpp` — honour it (`markNotSpillable`)

RISC-V backend:
* `RISCVInstrInfoXAIcore.td` (new) — `FeatureVendorXAIcore`, 4 register classes, 82 instructions/pseudos
* `XAIcoreLowering.{h,cpp}` (new) — `__yai_*` call → machine node lowering (879 lines)
* `RISCVInstrInfo.td`, `RISCVISelLowering.cpp`, `RISCVISelDAGToDAG.cpp`,
  `RISCVInstrInfo.cpp`, `RISCVRegisterInfo.{h,cpp}`, `RISCVCallingConv.cpp`,
  `RISCVPostRAExpandPseudoInsts.cpp`, `MCTargetDesc/RISCVInstPrinter.{h,cpp}`,
  `Disassembler/RISCVDisassembler.cpp`, `CMakeLists.txt`

Clang:
* `clang/include/clang/Basic/BuiltinsRISCV.td`, `clang/lib/CodeGen/TargetBuiltins/RISCV.cpp`

Tests: `llvm/test/CodeGen/RISCV/xaicore-*.ll` (16 files)

## Invariants the extension relies on

1. Tile/Acc/Vec values must not be spilled (no load/store encoding); the RA is told via
   `isSpillableForVirtReg` / `shouldRegionSplitForVirtReg`, and `storeRegToStackSlot`
   reports a fatal error.
2. Values must not cross the C ABI (`RISCVCallingConv.cpp` reports a fatal error);
   the four classes are added to the call-preserved mask so they survive calls to
   scalar soft-float helpers.
3. Field selectors, `mma` zero/lt/rt flags and mask fill codes must be compile-time
   constants (validated in the lowering).
4. A newly defined value needs its descriptor programmed before use; the lowering
   emits `cfg.copy` / `cfg.setx` for that (`*_create` pseudos expand to `IMPLICIT_DEF`).

## Build

```bash
cmake -G Ninja -S llvm -B build \
  -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=RISCV \
  -DLLVM_ENABLE_ZSTD=OFF
ninja -C build clang llc
```

Use it with `-march=rv64im_xaicore` (the builtins are gated on the `xaicore` feature).

## Notice (Apache-2.0 WITH LLVM-exception)

This branch contains modifications by the YAI Tile NPU project on top of LLVM
21.1.7. Modified files are listed in the commit history of this branch
(`git log --stat llvmorg-21.1.7..HEAD`). See `LICENSE.TXT` for the license.
