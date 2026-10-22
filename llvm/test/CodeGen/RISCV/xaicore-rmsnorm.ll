; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; rmsnorm 用到的指令族（一个 32x32 f32 块）：
;   vec32 配置/fill、vload、areduce.rows.sumsq（覆盖写，所以合并要
;   cfg.copy + reduce + vadd）、vmulx/vaddx/vrsqrt、amulv.byrow/bycol。

; CHECK-LABEL: rmsnorm_one_block:
; CHECK: cfg.setx vc{{[0-9]+}}, dtype,
; CHECK: cfg.setx vc{{[0-9]+}}, len,
; CHECK: vfillx v{{[0-9]+}}, {{.*}}
; CHECK: cfg.setx am{{[0-9]+}}, base_addr,
; CHECK: cfg.setx am{{[0-9]+}}, row_stride_bytes,
; CHECK: cfg.setx am{{[0-9]+}}, col_stride_bytes,
; CHECK: aload
; CHECK: cfg.copy vc{{[0-9]+}}, vc{{[0-9]+}}
; CHECK: areduce.rows.sumsq v{{[0-9]+}}, a{{[0-9]+}}
; CHECK: vadd v{{[0-9]+}}, v{{[0-9]+}}, v{{[0-9]+}}
; CHECK: vmulx v{{[0-9]+}}, v{{[0-9]+}}, {{.*}}
; CHECK: vaddx v{{[0-9]+}}, v{{[0-9]+}}, {{.*}}
; CHECK: vrsqrt.approx v{{[0-9]+}}, v{{[0-9]+}}
; CHECK: cfg.setx vm{{[0-9]+}}, base_addr,
; CHECK: cfg.setx vm{{[0-9]+}}, stride_bytes,
; CHECK: cfg.setx vc{{[0-9]+}}, len,
; CHECK: cfg.setx vc{{[0-9]+}}, dtype,
; CHECK: vload v{{[0-9]+}}
; CHECK: amulv.byrow a{{[0-9]+}}, a{{[0-9]+}}, v{{[0-9]+}}
; CHECK: amulv.bycol a{{[0-9]+}}, a{{[0-9]+}}, v{{[0-9]+}}
; CHECK: astore
; CHECK-NOT: __yai_
; CHECK: ret

define void @rmsnorm_one_block(ptr %x, ptr %w) {
entry:
  %sum = call <32 x i32> @__yai_vec32_create()
  call void @__yai_vec32_set_dtype(<32 x i32> %sum, i32 4)
  call void @__yai_vec32_set_valid_zone(<32 x i32> %sum, i32 32)
  call void @__yai_vec32_fill(<32 x i32> %sum, float 0.0)
  %a = call <1024 x i32> @__yai_acc_create()
  call void @__yai_acc_set_data(<1024 x i32> %a, ptr %x)
  call void @__yai_acc_set_strides(<1024 x i32> %a, i32 128, i32 4)
  call void @__yai_acc_set_valid_zone_pair(<1024 x i32> %a, i8 32, i8 32)
  call void @__yai_acc_set_dtype(<1024 x i32> %a, i32 4)
  call void @__yai_acc_load(<1024 x i32> %a)
  call void @__yai_acc_row_sum_sq_merge(<1024 x i32> %a, <32 x i32> %sum)
  call void @__yai_vec32_mul_scalar(<32 x i32> %sum, float 1.000000e+00)
  call void @__yai_vec32_add_scalar(<32 x i32> %sum, float 1.000000e+00)
  call void @__yai_vec32_rsqrt(<32 x i32> %sum)
  %wv = call <32 x i32> @__yai_vec32_create()
  call void @__yai_vec32_set_data(<32 x i32> %wv, ptr %w)
  call void @__yai_vec32_set_stride(<32 x i32> %wv, i32 4)
  call void @__yai_vec32_set_valid_zone(<32 x i32> %wv, i32 32)
  call void @__yai_vec32_set_dtype(<32 x i32> %wv, i32 4)
  call void @__yai_vec32_load(<32 x i32> %wv)
  call void @__yai_acc_mul_vec(<1024 x i32> %a, <32 x i32> %sum, i1 true)
  call void @__yai_acc_mul_vec(<1024 x i32> %a, <32 x i32> %wv, i1 false)
  call void @__yai_acc_store(<1024 x i32> %a)
  ret void
}

declare <32 x i32> @__yai_vec32_create()
declare <1024 x i32> @__yai_acc_create()
declare void @__yai_vec32_set_dtype(<32 x i32>, i32)
declare void @__yai_vec32_set_valid_zone(<32 x i32>, i32)
declare void @__yai_vec32_fill(<32 x i32>, float)
declare void @__yai_vec32_set_data(<32 x i32>, ptr)
declare void @__yai_vec32_set_stride(<32 x i32>, i32)
declare void @__yai_vec32_load(<32 x i32>)
declare void @__yai_vec32_mul_scalar(<32 x i32>, float)
declare void @__yai_vec32_add_scalar(<32 x i32>, float)
declare void @__yai_vec32_rsqrt(<32 x i32>)
declare void @__yai_acc_set_data(<1024 x i32>, ptr)
declare void @__yai_acc_set_strides(<1024 x i32>, i32, i32)
declare void @__yai_acc_set_valid_zone_pair(<1024 x i32>, i8, i8)
declare void @__yai_acc_set_dtype(<1024 x i32>, i32)
declare void @__yai_acc_load(<1024 x i32>)
declare void @__yai_acc_store(<1024 x i32>)
declare void @__yai_acc_row_sum_sq_merge(<1024 x i32>, <32 x i32>)
declare void @__yai_acc_mul_vec(<1024 x i32>, <32 x i32>, i1)
