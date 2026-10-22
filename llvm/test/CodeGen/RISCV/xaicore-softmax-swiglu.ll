; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; softmax / swiglu 用到的指令族：
;   areduce.rows.max/sum（覆盖写，合并要 cfg.copy + reduce + vmax/vadd）
;   asubv/adivv.byrow、amulx(log2e)+aexp2、asilu.approx、aabs
;   afillx、aaddx/amulx、vec32 的 vadd/vsub/vmul/vdiv/vmax、vmulx+vexp2

; CHECK-LABEL: softmax_swiglu:
; CHECK: cfg.copy vc{{[0-9]+}}, vc{{[0-9]+}}
; CHECK: areduce.rows.max v{{[0-9]+}}, a{{[0-9]+}}
; CHECK: vmax v{{[0-9]+}}, v{{[0-9]+}}, v{{[0-9]+}}
; CHECK: asubv.byrow a{{[0-9]+}}, a{{[0-9]+}}, v{{[0-9]+}}
; CHECK: amulx a{{[0-9]+}}, a{{[0-9]+}}, {{.*}}
; CHECK: aexp2.approx a{{[0-9]+}}, a{{[0-9]+}}
; CHECK: cfg.copy vc{{[0-9]+}}, vc{{[0-9]+}}
; CHECK: areduce.rows.sum v{{[0-9]+}}, a{{[0-9]+}}
; CHECK: vadd v{{[0-9]+}}, v{{[0-9]+}}, v{{[0-9]+}}
; CHECK: adivv.byrow a{{[0-9]+}}, a{{[0-9]+}}, v{{[0-9]+}}
; CHECK: asilu.approx a{{[0-9]+}}, a{{[0-9]+}}
; CHECK: aabs a{{[0-9]+}}, a{{[0-9]+}}
; CHECK: afillx a{{[0-9]+}}, {{.*}}
; CHECK: aaddx a{{[0-9]+}}, a{{[0-9]+}}, {{.*}}
; CHECK: vadd v{{[0-9]+}}, v{{[0-9]+}}, v{{[0-9]+}}
; CHECK: vsub v{{[0-9]+}}, v{{[0-9]+}}, v{{[0-9]+}}
; CHECK: vmul v{{[0-9]+}}, v{{[0-9]+}}, v{{[0-9]+}}
; CHECK: vdiv v{{[0-9]+}}, v{{[0-9]+}}, v{{[0-9]+}}
; CHECK: vmax v{{[0-9]+}}, v{{[0-9]+}}, v{{[0-9]+}}
; CHECK: vmulx v{{[0-9]+}}, v{{[0-9]+}}, {{.*}}
; CHECK: vexp2.approx v{{[0-9]+}}, v{{[0-9]+}}
; CHECK: astore
; CHECK-NOT: __yai_
; CHECK: ret

define void @softmax_swiglu(ptr %x, ptr %v) {
entry:
  %m = call <32 x i32> @__yai_vec32_create()
  call void @__yai_vec32_set_dtype(<32 x i32> %m, i32 4)
  call void @__yai_vec32_set_valid_zone(<32 x i32> %m, i32 32)
  call void @__yai_vec32_fill(<32 x i32> %m, float -1.000000e+00)
  %n = call <32 x i32> @__yai_vec32_create()
  call void @__yai_vec32_set_dtype(<32 x i32> %n, i32 4)
  call void @__yai_vec32_set_valid_zone(<32 x i32> %n, i32 32)
  call void @__yai_vec32_fill(<32 x i32> %n, float 0.000000e+00)

  %a = call <1024 x i32> @__yai_acc_create()
  call void @__yai_acc_set_data(<1024 x i32> %a, ptr %x)
  call void @__yai_acc_set_strides(<1024 x i32> %a, i32 128, i32 4)
  call void @__yai_acc_set_valid_zone_pair(<1024 x i32> %a, i8 32, i8 32)
  call void @__yai_acc_set_dtype(<1024 x i32> %a, i32 4)
  call void @__yai_acc_load(<1024 x i32> %a)

  call void @__yai_acc_row_max_merge(<1024 x i32> %a, <32 x i32> %m)
  call void @__yai_acc_sub_vec(<1024 x i32> %a, <32 x i32> %m, i1 true)
  call void @__yai_acc_exp(<1024 x i32> %a)
  call void @__yai_acc_row_sum_merge(<1024 x i32> %a, <32 x i32> %n)
  call void @__yai_acc_div_vec(<1024 x i32> %a, <32 x i32> %n, i1 true)
  call void @__yai_acc_silu(<1024 x i32> %a)
  call void @__yai_acc_abs(<1024 x i32> %a)
  call void @__yai_acc_fill(<1024 x i32> %a, float 0.000000e+00)
  call void @__yai_acc_add_scalar(<1024 x i32> %a, float 1.000000e+00)

  call void @__yai_vec32_add(<32 x i32> %m, <32 x i32> %n)
  call void @__yai_vec32_sub(<32 x i32> %m, <32 x i32> %n)
  call void @__yai_vec32_mul(<32 x i32> %m, <32 x i32> %n)
  call void @__yai_vec32_div(<32 x i32> %m, <32 x i32> %n)
  call void @__yai_vec32_max(<32 x i32> %m, <32 x i32> %n)
  call void @__yai_vec32_exp(<32 x i32> %m)

  call void @__yai_acc_store(<1024 x i32> %a)
  ret void
}

declare <32 x i32> @__yai_vec32_create()
declare <1024 x i32> @__yai_acc_create()
declare void @__yai_vec32_set_dtype(<32 x i32>, i32)
declare void @__yai_vec32_set_valid_zone(<32 x i32>, i32)
declare void @__yai_vec32_fill(<32 x i32>, float)
declare void @__yai_vec32_add(<32 x i32>, <32 x i32>)
declare void @__yai_vec32_sub(<32 x i32>, <32 x i32>)
declare void @__yai_vec32_mul(<32 x i32>, <32 x i32>)
declare void @__yai_vec32_div(<32 x i32>, <32 x i32>)
declare void @__yai_vec32_max(<32 x i32>, <32 x i32>)
declare void @__yai_vec32_exp(<32 x i32>)
declare void @__yai_acc_set_data(<1024 x i32>, ptr)
declare void @__yai_acc_set_strides(<1024 x i32>, i32, i32)
declare void @__yai_acc_set_valid_zone_pair(<1024 x i32>, i8, i8)
declare void @__yai_acc_set_dtype(<1024 x i32>, i32)
declare void @__yai_acc_load(<1024 x i32>)
declare void @__yai_acc_store(<1024 x i32>)
declare void @__yai_acc_row_max_merge(<1024 x i32>, <32 x i32>)
declare void @__yai_acc_row_sum_merge(<1024 x i32>, <32 x i32>)
declare void @__yai_acc_sub_vec(<1024 x i32>, <32 x i32>, i1)
declare void @__yai_acc_div_vec(<1024 x i32>, <32 x i32>, i1)
declare void @__yai_acc_exp(<1024 x i32>)
declare void @__yai_acc_silu(<1024 x i32>)
declare void @__yai_acc_abs(<1024 x i32>)
declare void @__yai_acc_fill(<1024 x i32>, float)
declare void @__yai_acc_add_scalar(<1024 x i32>, float)
