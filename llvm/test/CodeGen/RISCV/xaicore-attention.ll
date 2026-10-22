; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; attention 的收尾原语：
;   * 产生新 Vec32 的归约：先 cfg.get ac.rows，再把新 vc 的 dtype/len 配好，
;     最后 areduce.rows.{max,sum,absmax}；
;   * amaskx.tril（fill5 立即数，-1.0 -> 码 2）；
;   * tquant.rows.q8s32。

; CHECK-LABEL: attention_pieces:
; CHECK: cfg.get [[R1:[a-z0-9]+]], ac{{[0-9]+}}, rows
; CHECK: cfg.setx vc{{[0-9]+}}, dtype,
; CHECK: cfg.setx vc{{[0-9]+}}, len, [[R1]]
; CHECK: areduce.rows.max v{{[0-9]+}}, a{{[0-9]+}}
; CHECK: asubv.byrow a{{[0-9]+}}, a{{[0-9]+}}, v{{[0-9]+}}
; CHECK: amulx a{{[0-9]+}}, a{{[0-9]+}}, {{.*}}
; CHECK: aexp2.approx a{{[0-9]+}}, a{{[0-9]+}}
; CHECK: cfg.get [[R2:[a-z0-9]+]], ac{{[0-9]+}}, rows
; CHECK: cfg.setx vc{{[0-9]+}}, dtype,
; CHECK: cfg.setx vc{{[0-9]+}}, len, [[R2]]
; CHECK: areduce.rows.sum v{{[0-9]+}}, a{{[0-9]+}}
; CHECK: adivv.byrow a{{[0-9]+}}, a{{[0-9]+}}, v{{[0-9]+}}
; CHECK: cfg.get [[R3:[a-z0-9]+]], ac{{[0-9]+}}, rows
; CHECK: areduce.rows.absmax v{{[0-9]+}}, a{{[0-9]+}}
; CHECK: tquant.rows.q8s32 t{{[0-9]+}}, v{{[0-9]+}}, a{{[0-9]+}}
; CHECK: amaskx.tril a{{[0-9]+}}, a{{[0-9]+}}, {{.*}}, 2
; CHECK: cfg.copy vc{{[0-9]+}}, vc{{[0-9]+}}
; CHECK: areduce.rows.absmax v{{[0-9]+}}, a{{[0-9]+}}
; CHECK: vmax v{{[0-9]+}}, v{{[0-9]+}}, v{{[0-9]+}}
; CHECK-NOT: __yai_
; CHECK: ret

define void @attention_pieces(ptr %x) {
entry:
  %a = call <1024 x i32> @__yai_acc_create()
  call void @__yai_acc_set_data(<1024 x i32> %a, ptr %x)
  call void @__yai_acc_set_strides(<1024 x i32> %a, i32 128, i32 4)
  call void @__yai_acc_set_valid_zone_pair(<1024 x i32> %a, i8 32, i8 32)
  call void @__yai_acc_set_dtype(<1024 x i32> %a, i32 4)
  call void @__yai_acc_load(<1024 x i32> %a)

  %m = call <32 x i32> @__yai_acc_row_max(<1024 x i32> %a)
  call void @__yai_acc_sub_vec(<1024 x i32> %a, <32 x i32> %m, i1 true)
  call void @__yai_acc_exp(<1024 x i32> %a)
  %s = call <32 x i32> @__yai_acc_row_sum(<1024 x i32> %a)
  call void @__yai_acc_div_vec(<1024 x i32> %a, <32 x i32> %s, i1 true)
  %mx = call <32 x i32> @__yai_acc_row_absmax(<1024 x i32> %a)

  %t = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_set_dtype(<1024 x i8> %t, i32 0)
  call void @__yai_tile_set_valid_zone_pair(<1024 x i8> %t, i8 32, i8 32)
  call void @__yai_acc_quant(<1024 x i32> %a, <32 x i32> %mx, <1024 x i8> %t)

  call void @__yai_acc_mask(<1024 x i32> %a, i32 0, float -1.000000e+00)
  call void @__yai_acc_row_absmax_merge(<1024 x i32> %a, <32 x i32> %mx)
  ret void
}

declare <1024 x i32> @__yai_acc_create()
declare <1024 x i8> @__yai_tile_create()
declare <32 x i32> @__yai_acc_row_max(<1024 x i32>)
declare <32 x i32> @__yai_acc_row_sum(<1024 x i32>)
declare <32 x i32> @__yai_acc_row_absmax(<1024 x i32>)
declare void @__yai_acc_set_data(<1024 x i32>, ptr)
declare void @__yai_acc_set_strides(<1024 x i32>, i32, i32)
declare void @__yai_acc_set_valid_zone_pair(<1024 x i32>, i8, i8)
declare void @__yai_acc_set_dtype(<1024 x i32>, i32)
declare void @__yai_acc_load(<1024 x i32>)
declare void @__yai_acc_sub_vec(<1024 x i32>, <32 x i32>, i1)
declare void @__yai_acc_div_vec(<1024 x i32>, <32 x i32>, i1)
declare void @__yai_acc_exp(<1024 x i32>)
declare void @__yai_acc_quant(<1024 x i32>, <32 x i32>, <1024 x i8>)
declare void @__yai_acc_mask(<1024 x i32>, i32, float)
declare void @__yai_acc_row_absmax_merge(<1024 x i32>, <32 x i32>)
declare void @__yai_tile_set_dtype(<1024 x i8>, i32)
declare void @__yai_tile_set_valid_zone_pair(<1024 x i8>, i8, i8)
