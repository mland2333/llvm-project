; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; Vec32 / Vec8 的 create 只做寄存器分配：没有对应的机器指令
; （post-RA 展开成 IMPLICIT_DEF，不产生任何机器码）。这里验证它不会凭空
; 生成 vcopy/bcopy 之类的指令。

; CHECK-LABEL: vec_create:
; CHECK-NOT: vcopy
; CHECK-NOT: bcopy
; CHECK: ret

define void @vec_create() {
entry:
  %v = call <32 x i32> @__yai_vec32_create()
  %b = call <32 x i8> @__yai_vec8_create()
  ret void
}

declare <32 x i32> @__yai_vec32_create()
declare <32 x i8> @__yai_vec8_create()
