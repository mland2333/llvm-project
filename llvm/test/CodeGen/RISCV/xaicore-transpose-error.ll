; RUN: not llc -mtriple=riscv64 -mattr=+xaicore < %s 2>&1 | FileCheck %s

; 只支持非转置的 mma.nn；转置形式（lt/rt 为 true）给出明确错误。
;
; CHECK: only nn/nt mma variants are supported (left operand must not be transposed)

define void @transposed(ptr %p) {
entry:
  %a = call <1024 x i8> @__yai_tile_create()
  %b = call <1024 x i8> @__yai_tile_create()
  %acc = call <1024 x i32> @__yai_acc_create()
  call void @__yai_tile_matmul(<1024 x i8> %a, <1024 x i8> %b,
                               <1024 x i32> %acc, i1 true, i1 true, i1 false)
  ret void
}

declare <1024 x i8> @__yai_tile_create()
declare <1024 x i32> @__yai_acc_create()
declare void @__yai_tile_matmul(<1024 x i8>, <1024 x i8>, <1024 x i32>, i1, i1, i1)
