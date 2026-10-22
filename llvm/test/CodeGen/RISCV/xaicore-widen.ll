; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; twiden（Tile i8 -> Acc i32 原地 widen）和 vcopy（def 形态的数据拷贝）。

; CHECK-LABEL: widen_copy:
; CHECK: twiden.i8.i32 a{{[0-9]+}}, t{{[0-9]+}}
; CHECK: acvt.i32.f32 a{{[0-9]+}}, a{{[0-9]+}}
; CHECK: vcopy v{{[0-9]+}}, v{{[0-9]+}}
; CHECK: vstore
; CHECK-NOT: __yai_
; CHECK: ret

define void @widen_copy(ptr %tab, ptr %out, ptr %vin, ptr %vout) {
entry:
  %t = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_set_dtype(<1024 x i8> %t, i32 0)
  call void @__yai_tile_set_valid_zone_pair(<1024 x i8> %t, i8 32, i8 32)
  call void @__yai_tile_set_data(<1024 x i8> %t, ptr %tab)
  call void @__yai_tile_set_strides(<1024 x i8> %t, i32 32, i32 1)
  call void @__yai_tile_load(<1024 x i8> %t)

  %w = call <1024 x i32> @__yai_acc_create()
  call void @__yai_acc_set_dtype(<1024 x i32> %w, i32 2)
  call void @__yai_acc_set_valid_zone_pair(<1024 x i32> %w, i8 32, i8 32)
  call void @__yai_tile_widen(<1024 x i8> %t, <1024 x i32> %w)
  call void @__yai_acc_to_fp32(<1024 x i32> %w)
  call void @__yai_acc_set_data(<1024 x i32> %w, ptr %out)
  call void @__yai_acc_set_strides(<1024 x i32> %w, i32 128, i32 4)
  call void @__yai_acc_store(<1024 x i32> %w)

  %v = call <32 x i32> @__yai_vec32_create()
  call void @__yai_vec32_set_dtype(<32 x i32> %v, i32 4)
  call void @__yai_vec32_set_valid_zone(<32 x i32> %v, i32 32)
  call void @__yai_vec32_set_data(<32 x i32> %v, ptr %vin)
  call void @__yai_vec32_set_stride(<32 x i32> %v, i32 4)
  call void @__yai_vec32_load(<32 x i32> %v)
  %c = call <32 x i32> @__yai_vec32_copy(<32 x i32> %v)
  call void @__yai_vec32_set_data(<32 x i32> %c, ptr %vout)
  call void @__yai_vec32_set_stride(<32 x i32> %c, i32 4)
  call void @__yai_vec32_store(<32 x i32> %c)
  ret void
}

declare <1024 x i8> @__yai_tile_create()
declare <1024 x i32> @__yai_acc_create()
declare <32 x i32> @__yai_vec32_create()
declare void @__yai_tile_set_dtype(<1024 x i8>, i32)
declare void @__yai_tile_set_valid_zone_pair(<1024 x i8>, i8, i8)
declare void @__yai_tile_set_data(<1024 x i8>, ptr)
declare void @__yai_tile_set_strides(<1024 x i8>, i32, i32)
declare void @__yai_tile_load(<1024 x i8>)
declare void @__yai_tile_widen(<1024 x i8>, <1024 x i32>)
declare void @__yai_acc_set_dtype(<1024 x i32>, i32)
declare void @__yai_acc_set_valid_zone_pair(<1024 x i32>, i8, i8)
declare void @__yai_acc_to_fp32(<1024 x i32>)
declare void @__yai_acc_set_data(<1024 x i32>, ptr)
declare void @__yai_acc_set_strides(<1024 x i32>, i32, i32)
declare void @__yai_acc_store(<1024 x i32>)
declare void @__yai_vec32_set_dtype(<32 x i32>, i32)
declare void @__yai_vec32_set_valid_zone(<32 x i32>, i32)
declare void @__yai_vec32_set_data(<32 x i32>, ptr)
declare void @__yai_vec32_set_stride(<32 x i32>, i32)
declare void @__yai_vec32_load(<32 x i32>)
declare <32 x i32> @__yai_vec32_copy(<32 x i32>)
declare void @__yai_vec32_store(<32 x i32>)
