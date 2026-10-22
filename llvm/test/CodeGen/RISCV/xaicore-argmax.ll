; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; 行 argmax（采样）：areduce.rows.argmax 的输出是 i32 向量，
; 所以要先把新 vc 的 dtype 配成 2（i32）、len = ac.rows。

; CHECK-LABEL: argmax:
; CHECK: cfg.get [[R:[a-z0-9]+]], ac{{[0-9]+}}, rows
; CHECK: cfg.setx vc{{[0-9]+}}, dtype,
; CHECK: cfg.setx vc{{[0-9]+}}, len, [[R]]
; CHECK: areduce.rows.argmax v{{[0-9]+}}, a{{[0-9]+}}
; CHECK: vstore
; CHECK-NOT: __yai_
; CHECK: ret

define void @argmax(ptr %logits, ptr %idx) {
entry:
  %a = call <1024 x i32> @__yai_acc_create()
  call void @__yai_acc_set_data(<1024 x i32> %a, ptr %logits)
  call void @__yai_acc_set_strides(<1024 x i32> %a, i32 128, i32 4)
  call void @__yai_acc_set_valid_zone_pair(<1024 x i32> %a, i8 32, i8 32)
  call void @__yai_acc_set_dtype(<1024 x i32> %a, i32 4)
  call void @__yai_acc_load(<1024 x i32> %a)

  %v = call <32 x i32> @__yai_acc_row_argmax(<1024 x i32> %a)
  call void @__yai_vec32_set_data(<32 x i32> %v, ptr %idx)
  call void @__yai_vec32_set_stride(<32 x i32> %v, i32 4)
  call void @__yai_vec32_store(<32 x i32> %v)
  ret void
}

declare <1024 x i32> @__yai_acc_create()
declare void @__yai_acc_set_data(<1024 x i32>, ptr)
declare void @__yai_acc_set_strides(<1024 x i32>, i32, i32)
declare void @__yai_acc_set_valid_zone_pair(<1024 x i32>, i8, i8)
declare void @__yai_acc_set_dtype(<1024 x i32>, i32)
declare void @__yai_acc_load(<1024 x i32>)
declare <32 x i32> @__yai_acc_row_argmax(<1024 x i32>)
declare void @__yai_vec32_set_data(<32 x i32>, ptr)
declare void @__yai_vec32_set_stride(<32 x i32>, i32)
declare void @__yai_vec32_store(<32 x i32>)
