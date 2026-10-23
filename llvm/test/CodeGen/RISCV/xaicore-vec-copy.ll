; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; __yai_vec32_copy 是少数"有 def"的指令之一（vcopy 的目的寄存器不是输入）。
; 它的 def 必须声明成 DAG 结果（lowering 里的 emitDef），否则 InstrEmitter 的
; optional-def 逻辑会让生成的 MachineInstr 只有 1 个操作数，而描述符有 2 个，
; 于是任何按 getDesc().getNumOperands() 遍历的 pass（MachineLICM 就是）越界崩溃。
;
; 这里验证运行时循环里的 vcopy 仍然是"目的 + 源"两个操作数。

; CHECK-LABEL: vec_copy_loop:
; CHECK: vcopy v{{[0-9]+}}, v{{[0-9]+}}

define void @vec_copy_loop(i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %v = call <32 x i32> @__yai_vec32_create()
  %c = call <32 x i32> @__yai_vec32_copy(<32 x i32> %v)
  %i.next = add nuw nsw i32 %i, 1
  %cond = icmp slt i32 %i.next, %n
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

declare <32 x i32> @__yai_vec32_create()
declare <32 x i32> @__yai_vec32_copy(<32 x i32>)
