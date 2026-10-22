; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; 两个 Tile 值各自是独立的 SSA 值，寄存器分配器必须给它们不同的 XTT 寄存器；
; 每个 Tile 的配置、load、store 也只能作用在自己的寄存器上。
;
; 具体“两个寄存器不相同、且各自的 tload/tstore 用自己的编号”由
; check_fixed_t0.py 通过 cfg.setx tmN 与 tN 的编号匹配来断言
; （FileCheck 无法把 tm0 的 0 反引用到 t0）。

; CHECK-LABEL: two_tiles:
; CHECK: cfg.setx {{tm[0-9]+}}, base_addr,
; CHECK: cfg.setx {{tm[0-9]+}}, base_addr,
; CHECK: tload {{t[0-9]+}}
; CHECK: tload {{t[0-9]+}}
; CHECK: tstore {{t[0-9]+}}
; CHECK: tstore {{t[0-9]+}}
; CHECK-NOT: __yai_
; CHECK: ret

define void @two_tiles(ptr %p, ptr %q) {
entry:
  %a = call <1024 x i8> @__yai_tile_create()
  %b = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_set_data(<1024 x i8> %a, ptr %p)
  call void @__yai_tile_set_data(<1024 x i8> %b, ptr %q)
  call void @__yai_tile_load(<1024 x i8> %a)
  call void @__yai_tile_load(<1024 x i8> %b)
  call void @__yai_tile_store(<1024 x i8> %a)
  call void @__yai_tile_store(<1024 x i8> %b)
  ret void
}

declare <1024 x i8> @__yai_tile_create()
declare void @__yai_tile_set_data(<1024 x i8>, ptr)
declare void @__yai_tile_load(<1024 x i8>)
declare void @__yai_tile_store(<1024 x i8>)
