; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; 行 gather（embedding）：tgather tD, vIndex
;   dst[i][j] = table[index[i]][j]（表地址/跨步来自 dst tile 的 tm）

; CHECK-LABEL: gather:
; CHECK: cfg.setx vc{{[0-9]+}}, dtype,
; CHECK: cfg.setx vc{{[0-9]+}}, len,
; CHECK: cfg.setx vm{{[0-9]+}}, base_addr,
; CHECK: cfg.setx vm{{[0-9]+}}, stride_bytes,
; CHECK: vload
; CHECK: cfg.setx tm{{[0-9]+}}, base_addr,
; CHECK: cfg.setx tm{{[0-9]+}}, row_stride_bytes,
; CHECK: cfg.setx tm{{[0-9]+}}, col_stride_bytes,
; CHECK: tgather [[T:t[0-9]+]], [[V:v[0-9]+]]
; CHECK: cfg.setx tm{{[0-9]+}}, base_addr,
; CHECK: tstore [[T]]
; CHECK-NOT: __yai_
; CHECK: ret

define void @gather(ptr %table, ptr %index, ptr %out) {
entry:
  %idx = call <32 x i32> @__yai_vec32_create()
  call void @__yai_vec32_set_dtype(<32 x i32> %idx, i32 2)
  call void @__yai_vec32_set_valid_zone(<32 x i32> %idx, i32 32)
  call void @__yai_vec32_set_data(<32 x i32> %idx, ptr %index)
  call void @__yai_vec32_set_stride(<32 x i32> %idx, i32 4)
  call void @__yai_vec32_load(<32 x i32> %idx)

  %t = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_set_dtype(<1024 x i8> %t, i32 0)
  call void @__yai_tile_set_valid_zone_pair(<1024 x i8> %t, i8 32, i8 32)
  call void @__yai_tile_set_data(<1024 x i8> %t, ptr %table)
  call void @__yai_tile_set_strides(<1024 x i8> %t, i32 32, i32 1)
  call void @__yai_tile_gather_from_vec(<1024 x i8> %t, <32 x i32> %idx)

  call void @__yai_tile_set_data(<1024 x i8> %t, ptr %out)
  call void @__yai_tile_set_strides(<1024 x i8> %t, i32 32, i32 1)
  call void @__yai_tile_store(<1024 x i8> %t)
  ret void
}

declare <32 x i32> @__yai_vec32_create()
declare <1024 x i8> @__yai_tile_create()
declare void @__yai_vec32_set_dtype(<32 x i32>, i32)
declare void @__yai_vec32_set_valid_zone(<32 x i32>, i32)
declare void @__yai_vec32_set_data(<32 x i32>, ptr)
declare void @__yai_vec32_set_stride(<32 x i32>, i32)
declare void @__yai_vec32_load(<32 x i32>)
declare void @__yai_tile_set_dtype(<1024 x i8>, i32)
declare void @__yai_tile_set_valid_zone_pair(<1024 x i8>, i8, i8)
declare void @__yai_tile_set_data(<1024 x i8>, ptr)
declare void @__yai_tile_set_strides(<1024 x i8>, i32, i32)
declare void @__yai_tile_gather_from_vec(<1024 x i8>, <32 x i32>)
declare void @__yai_tile_store(<1024 x i8>)
