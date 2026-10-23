; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; cfg.setx <descriptor>, flags —— 访存后地址自增模式（ISA 已有，我们以前没接）。
;   tm/am: flags[1:0] = 0 关闭 / 1 按行(rows*row_stride) / 2 按列(cols*col_stride)
;   vm:    flags[0] = 1 时按 len*stride 自增
; 语义由模拟器 descriptor_view 的 incremented_base() 定义，只在访存成功后提交。

; CHECK-LABEL: tile_flags:
; CHECK: cfg.setx tm{{[0-9]+}}, flags
; CHECK: cfg.setx tm{{[0-9]+}}, base_addr
; CHECK: tload
; CHECK: tload
; CHECK-NOT: cfg.setx tm{{[0-9]+}}, base_addr
; CHECK: ret

define void @tile_flags(i64 %flags) {
  %t = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_set_strides(<1024 x i8> %t, i32 32, i32 1)
  call void @__yai_tile_set_valid_zone_pair(<1024 x i8> %t, i64 32, i64 32)
  call void @__yai_tile_set_dtype(<1024 x i8> %t, i32 0)
  call void @__yai_tile_set_flags(<1024 x i8> %t, i64 %flags)
  call void @__yai_tile_set_data(<1024 x i8> %t, ptr null)
  call void @__yai_tile_load(<1024 x i8> %t)
  call void @__yai_tile_load(<1024 x i8> %t)
  ret void
}

; CHECK-LABEL: vec_flags:
; CHECK: cfg.setx vm{{[0-9]+}}, flags
; CHECK: ret

define void @vec_flags(i64 %flags) {
  %v = call <32 x i32> @__yai_vec32_create()
  call void @__yai_vec32_set_dtype(<32 x i32> %v, i32 4)
  call void @__yai_vec32_set_valid_zone(<32 x i32> %v, i64 32)
  call void @__yai_vec32_set_stride(<32 x i32> %v, i32 4)
  call void @__yai_vec32_set_flags(<32 x i32> %v, i64 %flags)
  ret void
}

declare <1024 x i8> @__yai_tile_create()
declare void @__yai_tile_set_strides(<1024 x i8>, i32, i32)
declare void @__yai_tile_set_valid_zone_pair(<1024 x i8>, i64, i64)
declare void @__yai_tile_set_dtype(<1024 x i8>, i32)
declare void @__yai_tile_set_flags(<1024 x i8>, i64)
declare void @__yai_tile_set_data(<1024 x i8>, ptr)
declare void @__yai_tile_load(<1024 x i8>)
declare <32 x i32> @__yai_vec32_create()
declare void @__yai_vec32_set_dtype(<32 x i32>, i32)
declare void @__yai_vec32_set_valid_zone(<32 x i32>, i64)
declare void @__yai_vec32_set_stride(<32 x i32>, i32)
declare void @__yai_vec32_set_flags(<32 x i32>, i64)
