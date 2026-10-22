; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; 量化（per-row dynamic scale）：
;   tquant.rows.q8s32 tD, vScale, aS
;   scale 由指令自己算（absmax/127）并写回 vScale；q8 写进 tile。
; 结果分别用 tstore / vstore 写回内存。

; CHECK-LABEL: quant:
; CHECK: cfg.setx am{{[0-9]+}}, base_addr,
; CHECK: cfg.setx ac{{[0-9]+}}, dtype,
; CHECK: aload
; CHECK: cfg.setx vc{{[0-9]+}}, dtype,
; CHECK: cfg.setx vc{{[0-9]+}}, len,
; CHECK: cfg.setx tc{{[0-9]+}}, dtype,
; CHECK: cfg.setx tc{{[0-9]+}}, rows,
; CHECK: cfg.setx tc{{[0-9]+}}, cols,
; CHECK: tquant.rows.q8s32 [[T:t[0-9]+]], [[V:v[0-9]+]], [[A:a[0-9]+]]
; CHECK: tquantx.rows.q8s32 [[T]], [[V]], [[A]]
; CHECK: cfg.setx tm{{[0-9]+}}, base_addr,
; CHECK: cfg.setx tm{{[0-9]+}}, row_stride_bytes,
; CHECK: cfg.setx tm{{[0-9]+}}, col_stride_bytes,
; CHECK: tstore [[T]]
; CHECK: cfg.setx vm{{[0-9]+}}, base_addr,
; CHECK: cfg.setx vm{{[0-9]+}}, stride_bytes,
; CHECK: vstore [[V]]
; CHECK-NOT: __yai_
; CHECK: ret

define void @quant(ptr %x, ptr %q8, ptr %scale) {
entry:
  %a = call <1024 x i32> @__yai_acc_create()
  call void @__yai_acc_set_data(<1024 x i32> %a, ptr %x)
  call void @__yai_acc_set_strides(<1024 x i32> %a, i32 128, i32 4)
  call void @__yai_acc_set_valid_zone_pair(<1024 x i32> %a, i8 32, i8 32)
  call void @__yai_acc_set_dtype(<1024 x i32> %a, i32 4)
  call void @__yai_acc_load(<1024 x i32> %a)

  %s = call <32 x i32> @__yai_vec32_create()
  call void @__yai_vec32_set_dtype(<32 x i32> %s, i32 4)
  call void @__yai_vec32_set_valid_zone(<32 x i32> %s, i32 32)

  %t = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_set_dtype(<1024 x i8> %t, i32 0)
  call void @__yai_tile_set_valid_zone_pair(<1024 x i8> %t, i8 32, i8 32)

  call void @__yai_acc_quant(<1024 x i32> %a, <32 x i32> %s, <1024 x i8> %t)
  call void @__yai_acc_quant_ext(<1024 x i32> %a, <32 x i32> %s, <1024 x i8> %t)

  call void @__yai_tile_set_data(<1024 x i8> %t, ptr %q8)
  call void @__yai_tile_set_strides(<1024 x i8> %t, i32 32, i32 1)
  call void @__yai_tile_store(<1024 x i8> %t)

  call void @__yai_vec32_set_data(<32 x i32> %s, ptr %scale)
  call void @__yai_vec32_set_stride(<32 x i32> %s, i32 4)
  call void @__yai_vec32_store(<32 x i32> %s)
  ret void
}

declare <1024 x i32> @__yai_acc_create()
declare <32 x i32> @__yai_vec32_create()
declare <1024 x i8> @__yai_tile_create()
declare void @__yai_acc_set_data(<1024 x i32>, ptr)
declare void @__yai_acc_set_strides(<1024 x i32>, i32, i32)
declare void @__yai_acc_set_valid_zone_pair(<1024 x i32>, i8, i8)
declare void @__yai_acc_set_dtype(<1024 x i32>, i32)
declare void @__yai_acc_load(<1024 x i32>)
declare void @__yai_acc_quant(<1024 x i32>, <32 x i32>, <1024 x i8>)
declare void @__yai_acc_quant_ext(<1024 x i32>, <32 x i32>, <1024 x i8>)
declare void @__yai_vec32_set_dtype(<32 x i32>, i32)
declare void @__yai_vec32_set_valid_zone(<32 x i32>, i32)
declare void @__yai_vec32_set_data(<32 x i32>, ptr)
declare void @__yai_vec32_set_stride(<32 x i32>, i32)
declare void @__yai_vec32_store(<32 x i32>)
declare void @__yai_tile_set_dtype(<1024 x i8>, i32)
declare void @__yai_tile_set_valid_zone_pair(<1024 x i8>, i8, i8)
declare void @__yai_tile_set_data(<1024 x i8>, ptr)
declare void @__yai_tile_set_strides(<1024 x i8>, i32, i32)
declare void @__yai_tile_store(<1024 x i8>)
