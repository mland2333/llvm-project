# XAIcore 扩展：改动清单（逐文件说明）

本分支 = `llvm/llvm-project` **`llvmorg-21.1.7`**（commit `292dc2b86f66e39f4b85ec8b185fd8b60f5213ce`）
+ 一个 RISC-V vendor 扩展 `+xaicore`，让 C++ 通过 `__builtin_yai_*` 驱动一块独立的
tile 寄存器文件（YAI Tile NPU）。

统计：**36 个文件，+3346 / -7 行**（其中 20 个新文件）。

```
$ git diff --stat llvmorg-21.1.7..HEAD | tail -1
 36 files changed, 3346 insertions(+), 7 deletions(-)
```

---

## 0. 一张图看懂改动位置

```
C++ 算子代码
  __builtin_yai_tile_matmul(...)          ← ① clang 前端（2 个文件）
        │  降成普通 call __yai_tile_matmul(...)
        ▼
LLVM IR（无新类型、无 intrinsic）
        │  ② LowerCall 抢先拦截（RISCVISelLowering.cpp 的 1 处钩子）
        ▼
RISCV 机器指令 + 4 个新寄存器类            ← ③ 指令表 + lowering（3 个新文件）
        │  ④ 寄存器分配：不能 spill（2 个通用 CodeGen 文件 + 3 个 RISCV 文件）
        ▼
32-bit 定长编码 / 汇编文本 / 反汇编        ← ⑤ MC 层（3 个文件）
```

---

## 1. 通用 CodeGen（2 个文件，与目标无关的框架改动）

| 文件 | 增/删 | 作用 |
|---|---|---|
| `llvm/include/llvm/CodeGen/TargetRegisterInfo.h` | +9 | 新增虚钩子 `isSpillableForVirtReg(MF, LiveInterval)`，默认 `true`。给"寄存器类没有合法 load/store、无法溢出"的目标一个声明入口 |
| `llvm/lib/CodeGen/RegAllocGreedy.cpp` | +5 | 贪心分配器在溢出决策前调用上面的钩子；返回 `false` 时对 vreg 执行 `markNotSpillable()`，于是它**不会**被溢出，而是回退到重着色/分裂以外的手段 |

> 为什么必须是通用改动：LLVM 原生的 `shouldRegionSplitForVirtReg` 只能阻止"区域分裂"，
> 挡不住 `InlineSpiller` 真正插 spill。这两个文件让"不可溢出"成为一等公民。

---

## 2. RISC-V 后端（13 个文件，其中 3 个新文件）

| 文件 | 增/删 | 作用 |
|---|---|---|
| `RISCVInstrInfoXAIcore.td` **新** | +940 | 扩展本体：`FeatureVendorXAIcore`（`-march=+xaicore` 的门控）＋ 4 个寄存器类 ＋ 82 条指令/伪指令＋ 32-bit 定长编码 |
| `XAIcoreLowering.cpp` **新** | +879 | `__yai_*` call → 机器指令的核心：`StringSwitch` 分发；一对多展开（`acc_exp`→`amulx+aexp2`、归约 def/merge、`vec32_copy`→`cfg.copy+vcopy`）；常量校验；f32 标量取 GPR 低 32 位 |
| `XAIcoreLowering.h` **新** | +39 | 上述接口 `lowerXAIcoreBuiltinCall(Subtarget, CLI, InVals)` |
| `RISCVInstrInfo.td` | +1 | `include "RISCVInstrInfoXAIcore.td"` |
| `RISCVISelLowering.cpp` | +55 | ① `addRegisterClass` 注册 4 个 MVT；② `isXAIcoreValueType` + `getRegisterTypeForCallingConv`/`getNumRegistersForCallingConv`/`getVectorTypeBreakdownForCallingConv`（值整体占一个寄存器）；③ **`LowerCall` 开头**在 ABI 分配前拦截 `__yai_*` |
| `RISCVInstrInfo.cpp` | +32 | `copyPhysReg` 对 4 个类分别发 `tcopy/acopy/vcopy/bcopy`；`storeRegToStackSlot`/`reloadRegFromStackSlot` 遇到这 4 类直接 `report_fatal_error` |
| `RISCVRegisterInfo.{h,cpp}` | +8 / +62−7 | ① `isSpillableForVirtReg=false`、`shouldRegionSplitForVirtReg=false`；② `withXAIcorePreserved()` 把 4 个类加进 `getCallPreservedMask`（值可跨标量软浮点调用存活） |
| `RISCVCallingConv.cpp` | +14 | 若 YAI 值走到 C ABI 边界（缺少 `+xaicore` 或不该有的 ABI 边界）→ 带解释的 `report_fatal_error` |
| `RISCVPostRAExpandPseudoInsts.cpp` | +16 | `tile_create/acc_create/vec32_create/vec8_create` 伪指令 → `IMPLICIT_DEF`（不产生机器码） |
| `RISCVISelDAGToDAG.cpp` | +23 | 两个友好诊断：tile 值被 `store` 到内存、被 `select`（ISA 没有条件移动） |
| `MCTargetDesc/RISCVInstPrinter.{h,cpp}` | +24 / +100 | 12 个打印方法：`tN`/`tcN`/`tmN`、`aN`/`acN`/`amN`、`vN`/`vcN`/`vmN`、`bN`/`bcN`/`bmN` |
| `Disassembler/RISCVDisassembler.cpp` | +36 | `DecodeXTT/XTA/XTV/XTB` 四个寄存器类解码辅助（**尚未接线到解码表**，见 §5 已知限制） |
| `CMakeLists.txt` | +1 | 把 `XAIcoreLowering.cpp` 编进 RISCV 目标 |

### 2.1 四个寄存器类（`RISCVInstrInfoXAIcore.td`）

| 数据域 | LLVM 类型 | 类 | 个数 | 单个大小 |
|---|---|---|---|---|
| Tile | `<1024 x i8>` | `XTT` | 16 | 8 Kbit |
| Acc | `<1024 x i32>` | `XTA` | 16 | 32 Kbit |
| Vec32 | `<32 x i32>` | `XTV` | 32 | 1 Kbit |
| Vec8 | `<32 x i8>` | `XTB` | 32 | 256 bit |

关键设计：**描述符不是独立寄存器**。三个操作数类都是 `RegisterOperand<XTT>` 等，
只是 `PrintMethod` 不同：

```tablegen
def XTTOp  : RegisterOperand<XTT> { let PrintMethod = "printXAIcoreTileReg";    }  // tN
def XTTCOp : RegisterOperand<XTT> { let PrintMethod = "printXAIcoreTileConfig"; }  // tcN
def XTTMOp : RegisterOperand<XTT> { let PrintMethod = "printXAIcoreTileMemory"; }  // tmN
```

所以 `cfg.setx tc3, rows, 32` 与 `tload t3` 引用的是**同一个物理寄存器**，
"描述符跟着数据寄存器走"由 ordinary liveness 自动保证。

---

## 3. Clang 前端（2 个文件）

| 文件 | 增/删 | 作用 |
|---|---|---|
| `clang/include/clang/Basic/BuiltinsRISCV.td` | +124 | `class YAIBuiltin` + **110 个** `__builtin_yai_*` 声明：`CustomTypeChecking`（大向量参数由调用方自己检查）、`NoThrow`、`Features="xaicore"`（没有该扩展直接不可用） |
| `clang/lib/CodeGen/TargetBuiltins/RISCV.cpp` | +29 | `EmitRISCVBuiltinExpr` 识别 `__builtin_yai_` 前缀 → 用**真实 C++ 参数类型** `getOrInsertFunction("__yai_"+suffix)` → `CreateCall`。IR 里没有新类型/intrinsic，语义集中到后端一处 |

---

## 4. 测试（16 个新文件，`llvm/test/CodeGen/RISCV/`）

`llc -mtriple=riscv64 -mattr=+xaicore | FileCheck`，覆盖：

| 文件 | 覆盖 |
|---|---|
| `xaicore-matmul.ll` / `xaicore-matmul-nt.ll` | `mma.nn/nt` 的 zero/acc、K 块累加 + 双 scale epilogue |
| `xaicore-acc-ops.ll` | acc 逐元素（add/sub/mul/div、byrow/bycol 广播、fillx/abs/exp2/silu） |
| `xaicore-rmsnorm.ll` | sumsq 归约 + rsqrt + 两次 mul |
| `xaicore-softmax-swiglu.ll` | row max/sum 归约、exp2、div、mask |
| `xaicore-quant.ll` | `tquant/tquantx`、absmax 归约 |
| `xaicore-attention.ll` | 组合流水线（QK→mask→softmax→量化→PV） |
| `xaicore-argmax.ll` | `areduce.rows.argmax` + `cfg.get` 组 def 形态 |
| `xaicore-gather.ll` | `vgather/tgather` + `twiden` |
| `xaicore-widen.ll` | i8→i32 符号扩展 |
| `xaicore-vec-create.ll` | `*_create` 展开成 `IMPLICIT_DEF`（无机器码） |
| `xaicore-tile-phi.ll` | tile 值的 PHI（跨块存活） |
| `xaicore-two-tiles.ll` | 两个 tile 交错（寄存器分配 + 描述符不串） |
| `xaicore-fixed-t0.ll` | 固定物理寄存器的分配行为 |
| `xaicore-select-error.ll` / `xaicore-transpose-error.ll` | 两个负例：tile 值被 select、`lt=true` 左转置（应报错） |

---

## 5. 不变量（改动为什么这么写）

1. **值 = 一个寄存器**：4 个类无 alias/subreg，copy 是整寄存器（`copyPhysReg`）。
2. **不能溢出**：没有 load/store 编码 → `isSpillableForVirtReg` + fatal + 不切分三处拦截。
3. **必须保序**：所有 YAI 指令 `hasSideEffects=1`、`hasNoSchedulingInfo=1`，lowering 用 chain 串联（`cfg` 是全局状态）。
4. **标志位必须常量**：字段选择子、`mma` 的 zero/lt/rt、`mask` 的 fill 码在 lowering 里校验。
5. **新值要先配描述符**：`*_create` 定义寄存器但不配置；`vec32_copy`/归约 def 形态由 lowering 补发 `cfg.copy`/`cfg.setx`。
6. **不能过 C ABI**：走到 `RISCVCallingConv` 就 fatal；同时把 4 个类加入调用保持掩码，使值能跨软浮点 helper 调用存活。

## 6. 已知限制

| 项 | 状态 |
|---|---|
| AsmParser | 未做：不能手写 `.s` 再汇编（只能从 C/C++ 走 builtin） |
| Disassembler | 只有 `Decode*` 辅助函数，指令是 `InstFormatOther` 且没有 `DecoderMethod` → `llvm-objdump -d` 还认不出 YAI 指令 |
| 调度模型 | 无（`hasNoSchedulingInfo=1`）→ `MachineLICM` 的压力估计失真（`vcopy` 进运行时循环会崩，见下游仓库 `known_issues.md`） |
| `-O0` | 不支持：值会落到栈上，触发 "YAI tile value cannot be stored in memory" 诊断，要求 `-O1` |
| `select` / 左转置 mma | 明确报错（ISA 无条件移动、无 `lt` 编码） |
| IR 级优化 | 不参与：YAI 语义对中端不可见（这是"前端只发 call"的代价） |

## 7. 构建与验证

```bash
cmake -G Ninja -S llvm -B build \
  -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=RISCV -DLLVM_ENABLE_ZSTD=OFF
ninja -C build clang llc

# 后端单测
ninja -C build check-llvm-codegen-riscv     # 含 16 个 xaicore-*.ll

# 手工看一个用例
build/bin/llc -mtriple=riscv64 -mattr=+xaicore < llvm/test/CodeGen/RISCV/xaicore-matmul.ll \
  | build/bin/FileCheck llvm/test/CodeGen/RISCV/xaicore-matmul.ll
```

下游端到端（Qwen3-0.6B 真实推理，logits 与 host 参考逐位相同）见同组织的
`tile-npu` 仓库：`make check` / `make check-small` / `make big`。

## 8. 如何应用到别的 LLVM 版本

```bash
git checkout <你的基线 tag>
git am /path/to/0001-RISCV-Add-XAIcore-vendor-extension.patch    # 冲突主要在
#   RISCVInstrInfo.td / RISCVISelLowering.cpp / RISCVRegisterInfo.cpp / BuiltinsRISCV.td
```
