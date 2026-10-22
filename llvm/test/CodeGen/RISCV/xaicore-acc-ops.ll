; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; Acc 逐元素二元运算：C++ API 是原地运算（a.add(b)），所以 dest == src1。
;   aadd/asub/amul/adiv  aD, aD, aB
; 操作数都建模成 use（硬件原地写，SSA 身份不变）。

; CHECK-LABEL: acc_ops:
; CHECK: aadd [[A:a[0-9]+]], [[A]], [[B:a[0-9]+]]
; CHECK: asub [[A]], [[A]], [[B]]
; CHECK: amul [[A]], [[A]], [[B]]
; CHECK: adiv [[A]], [[A]], [[B]]
; CHECK: astore [[A]]
; CHECK-NOT: __yai_
; CHECK: ret

define void @acc_ops(ptr %p) {
entry:
  %a = call <1024 x i32> @__yai_acc_create()
  %b = call <1024 x i32> @__yai_acc_create()
  call void @__yai_acc_add_inplace(<1024 x i32> %a, <1024 x i32> %b)
  call void @__yai_acc_sub_inplace(<1024 x i32> %a, <1024 x i32> %b)
  call void @__yai_acc_mul_inplace(<1024 x i32> %a, <1024 x i32> %b)
  call void @__yai_acc_div_inplace(<1024 x i32> %a, <1024 x i32> %b)
  call void @__yai_acc_set_data(<1024 x i32> %a, ptr %p)
  call void @__yai_acc_store(<1024 x i32> %a)
  ret void
}

declare <1024 x i32> @__yai_acc_create()
declare void @__yai_acc_add_inplace(<1024 x i32>, <1024 x i32>)
declare void @__yai_acc_sub_inplace(<1024 x i32>, <1024 x i32>)
declare void @__yai_acc_mul_inplace(<1024 x i32>, <1024 x i32>)
declare void @__yai_acc_div_inplace(<1024 x i32>, <1024 x i32>)
declare void @__yai_acc_set_data(<1024 x i32>, ptr)
declare void @__yai_acc_store(<1024 x i32>)
