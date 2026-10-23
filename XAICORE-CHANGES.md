# XAIcore 扩展：改动清单（逐文件说明）

本分支 = `llvm/llvm-project` **`llvmorg-21.1.7`**（commit `292dc2b86f66e39f4b85ec8b185fd8b60f5213ce`）
+ 一个 RISC-V vendor 扩展 `+xaicore`，让 C++ 通过 `__builtin_yai_*` 驱动一块独立的
tile 寄存器文件（YAI Tile NPU）。

统计：**39 个文件，+3825 / -7 行**（其中 22 个新文件，18 个是后端测试；含本文件自身）。

```
$ git diff --stat 292dc2b86f..HEAD | tail -1
 39 files changed, 3825 insertions(+), 7 deletions(-)
```

> 分两批：`fd1abb016b` 的扩展本体（36 文件 / +3346）、以及之后 3 个修复/增强提交
> （+413，见 §9），外加本文件的说明更新。标签 `llvmorg-21.1.7` 在浅克隆里可能不存在，用基点 commit 比较等价。

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
| `RISCVInstrInfoXAIcore.td` **新** | +1039 | 扩展本体：`FeatureVendorXAIcore`（`-march=+xaicore` 的门控）＋ 4 个寄存器类 ＋ 82 条指令/伪指令＋ 32-bit 定长编码 |
| `XAIcoreLowering.cpp` **新** | +970 | `__yai_*` call → 机器指令的核心：`StringSwitch` 分发；一对多展开（`acc_exp`→`amulx+aexp2`、归约 def/merge、`vec32_copy`→`cfg.copy+vcopy`）；常量校验；f32 标量取 GPR 低 32 位 |
| `XAIcoreLowering.h` **新** | +39 | 上述接口 `lowerXAIcoreBuiltinCall(Subtarget, CLI, InVals)` |
| `RISCVInstrInfo.td` | +1 | `include "RISCVInstrInfoXAIcore.td"` |
| `RISCVISelLowering.cpp` | +55 | ① `addRegisterClass` 注册 4 个 MVT；② `isXAIcoreValueType` + `getRegisterTypeForCallingConv`/`getNumRegistersForCallingConv`/`getVectorTypeBreakdownForCallingConv`（值整体占一个寄存器）；③ **`LowerCall` 开头**在 ABI 分配前拦截 `__yai_*` |
| `RISCVInstrInfo.cpp` | +48 | `copyPhysReg` 对 4 个类**先发 `cfg.copy` 复制描述符、再发数据拷贝**（`tcopy/acopy/vcopy/bcopy`）；`storeRegToStackSlot`/`reloadRegFromStackSlot` 遇到这 4 类直接 `report_fatal_error` |
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

## 4. 测试（18 个新文件，`llvm/test/CodeGen/RISCV/`）

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
| `xaicore-vec-copy.ll` | `Vec32::copy` 落在**运行期循环**里（曾触发 `MachineLICM` 断言崩溃，见 §9.1） |
| `xaicore-cfg-flags.ll` | `cfg.setx …flags`；并断言打开自增后两次 `tload` 之间**不再**出现 `base_addr` 写入 |

---

## 5. 不变量（改动为什么这么写）

1. **值 = 一个寄存器**：4 个类无 alias/subreg，copy 是整寄存器；且**描述符是按物理寄存器号索引的旁路状态**，所以 `copyPhysReg` 必须先 `cfg.copy` 再搬数据（否则数据对、元数据陈旧）。
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
| 调度模型 | 仍无（`hasNoSchedulingInfo=1`）→ 中端看不到 YAI 语义、不做调度/重排。原本 `vcopy` 进运行时循环会崩，根因是 lowering 把 def 当操作数传（已修，见 §9.1） |
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

---

## 9. 后续修复与增强（3 个提交）

扩展落地后在真实内核（Qwen3-0.6B 的 tile 算子库）上跑出来的三个问题/缺口，各自独立成提交。

### 9.1 `[RISCV] XAIcore: emit vec copy through emitDef and assert the operand convention`

| 文件 | 增/删 | 作用 |
|---|---|---|
| `XAIcoreLowering.cpp` | +16 −2 | `CallVecCopyDef` 改用 `emitDef(...)`（def 作为 DAG **结果**）；`emit()`/`emitDef()` 各加一条 `getNumDefs()` 断言 |
| `xaicore-vec-copy.ll` **新** | +30 | 运行期循环里的 `Vec32::copy` |

**问题**：本目录的 `MachineSDNode` 约定是"defs = DAG 结果，operands = 仅 uses"，
而 `CallVecCopyDef` 把目的寄存器塞进了操作数列表 → 生成的指令只有 1 个操作数，而
模式声明的是 2 个（dst, src）。`MachineLICM::calcRegisterCost` 遍历操作数时
`ArrayRef` 越界断言失败（只要 `Vec32::copy` 落在运行期循环里就必崩）。

**为什么加断言**：这类错误的表现是"下游某个 pass 莫名崩溃"，加断言后变成
"编译期直接报错"，定位成本从半天降到一眼。

### 9.2 `[RISCV] XAIcore: copy the register descriptor in copyPhysReg`

| 文件 | 增/删 | 作用 |
|---|---|---|
| `RISCVInstrInfo.cpp` | +17 −1 | 4 个寄存器类的 `copyPhysReg` 先 `cfg.copy <dst>, <src>` 再搬数据 |
| `RISCVInstrInfoXAIcore.td` | +62 | 补 7 条 `cfg.copy`（TC/AC/BC/TM/AM/BM/VM；VC 原本就有） |
| `xaicore-tile-phi.ll` | +5 −2 | 要求 `cfg.copy` 出现在数据拷贝之前 |

**问题**：描述符（dtype/len/rows/cols、base/strides、flags）是**按物理寄存器号索引的
旁路状态**，不在数据里。`copyPhysReg` 只搬数据 → RA 插入的拷贝（coalescing 失败、
循环携带值）会得到"目的寄存器的陈旧元数据 + 源寄存器的数据"，表现为静默算错或
`invalid_configuration`。

编码：`0x70000002 | (reg_type << 25) | (1 << 22)`，`reg_type` 与模拟器的
`cfg_reg_type(sel2, vector_side)` 一致。

### 9.3 `[RISCV] XAIcore: expose the descriptor post-access increment (cfg.setx ... flags)`

| 文件 | 增/删 | 作用 |
|---|---|---|
| `BuiltinsRISCV.td` | +4 | `__builtin_yai_{tile,acc,vec32,vec8}_set_flags` |
| `RISCVInstrInfoXAIcore.td` | +37 | 4 条 `cfg.setx …, flags`（TM/AM/VM/BM） |
| `XAIcoreLowering.cpp` | +36 | 4 个 case（值与其它 `cfg.setx` 一样是运行期 GPR） |
| `xaicore-cfg-flags.ll` **新** | +52 | 含"自增打开后不再重写 base_addr"的断言 |

**硬件早就有、编译器没接**：ISA 的访存描述符支持访存后地址自增

```
tm/am.flags[1:0] = 0 关闭 / 1 按行(rows*row_stride_bytes) / 2 按列(cols*col_stride_bytes)
vm/bm.flags[0]   = 1 时 base += len*stride_bytes
只在访存成功后提交；gather 永不提交。
```

接入后内层循环只剩 `tload`：GEMM 的 k 循环从 36 条/迭代降到 **19 条**（-47%），
整模型（Qwen3-0.6B，32-token prefill）从 3,672,509 条降到 **2,319,041 条（-36.8%）**，
logits 逐位不变。

**注意它是跨指令状态**：换块必须重写 `base_addr`；尾块/非 32 倍数走显式地址的兜底
路径天然覆盖；`gather` 不受影响。
