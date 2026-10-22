; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; mma.nt（右侧转置，attention 的 QK^T）和 acvt（int32 -> f32）：
;   mma.nt.zero.i8.i32 aD, tA, tB
;   mma.nt.acc.i8.i32  aD, tA, tB
;   acvt.i32.f32       aD, aD
; lt=true 不支持（只允许 nn/nt），见 xaicore-transpose-error.ll。

; CHECK-LABEL: matmul_nt:
; CHECK: mma.nt.zero.i8.i32 [[AC:a[0-9]+]], {{t[0-9]+}}, {{t[0-9]+}}
; CHECK: mma.nt.acc.i8.i32 [[AC]], {{t[0-9]+}}, {{t[0-9]+}}
; CHECK: acvt.i32.f32 [[AC]], [[AC]]
; CHECK: astore [[AC]]
; CHECK-NOT: __yai_
; CHECK: ret

define void @matmul_nt(ptr %a, ptr %b, ptr %d, ptr %e, ptr %out) {
entry:
  %ta = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_set_data(<1024 x i8> %ta, ptr %a)
  call void @__yai_tile_set_strides(<1024 x i8> %ta, i32 32, i32 1)
  call void @__yai_tile_set_dtype(<1024 x i8> %ta, i32 0)
  call void @__yai_tile_set_valid_zone_pair(<1024 x i8> %ta, i8 32, i8 32)
  call void @__yai_tile_load(<1024 x i8> %ta)

  %tb = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_set_data(<1024 x i8> %tb, ptr %b)
  call void @__yai_tile_set_strides(<1024 x i8> %tb, i32 32, i32 1)
  call void @__yai_tile_set_dtype(<1024 x i8> %tb, i32 0)
  call void @__yai_tile_set_valid_zone_pair(<1024 x i8> %tb, i8 32, i8 32)
  call void @__yai_tile_load(<1024 x i8> %tb)

  %acc = call <1024 x i32> @__yai_acc_create()
  call void @__yai_acc_set_dtype(<1024 x i32> %acc, i32 2)
  call void @__yai_acc_set_valid_zone_pair(<1024 x i32> %acc, i8 32, i8 32)
  call void @__yai_tile_matmul(<1024 x i8> %ta, <1024 x i8> %tb,
                               <1024 x i32> %acc, i1 true, i1 false, i1 true)
  call void @__yai_tile_matmul(<1024 x i8> %ta, <1024 x i8> %tb,
                               <1024 x i32> %acc, i1 false, i1 false, i1 true)
  call void @__yai_acc_to_fp32(<1024 x i32> %acc)
  call void @__yai_acc_set_data(<1024 x i32> %acc, ptr %out)
  call void @__yai_acc_set_strides(<1024 x i32> %acc, i32 128, i32 4)
  call void @__yai_acc_store(<1024 x i32> %acc)
  ret void
}

declare <1024 x i8> @__yai_tile_create()
declare <1024 x i32> @__yai_acc_create()
declare void @__yai_tile_set_data(<1024 x i8>, ptr)
declare void @__yai_tile_set_strides(<1024 x i8>, i32, i32)
declare void @__yai_tile_set_dtype(<1024 x i8>, i32)
declare void @__yai_tile_set_valid_zone_pair(<1024 x i8>, i8, i8)
declare void @__yai_tile_load(<1024 x i8>)
declare void @__yai_acc_set_data(<1024 x i32>, ptr)
declare void @__yai_acc_set_strides(<1024 x i32>, i32, i32)
declare void @__yai_acc_set_dtype(<1024 x i32>, i32)
declare void @__yai_acc_set_valid_zone_pair(<1024 x i32>, i8, i8)
declare void @__yai_acc_to_fp32(<1024 x i32>)
declare void @__yai_acc_store(<1024 x i32>)
declare void @__yai_tile_matmul(<1024 x i8>, <1024 x i8>, <1024 x i32>, i1, i1, i1)
