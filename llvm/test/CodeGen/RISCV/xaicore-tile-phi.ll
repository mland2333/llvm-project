; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; Tile 值跨基本块 PHI 合流：分配器必须把两个来源合并到一个 XTT 寄存器，
; 并在需要时插入 tcopy（ISA 的 Tile 拷贝指令）。这里验证：
;   * 两个来源的配置/load 各出现一次；
;   * 合流点只有一个 tload，作用在合并后的寄存器上；
;   * 某个分支出现真正的 tcopy $dst, $src（RA 为 PHI 插入的拷贝）。
;
; CHECK-LABEL: phi_tile:
; CHECK: bnez
; CHECK: tcopy [[A:t[0-9]+]], [[B:t[0-9]+]]
; CHECK: tload [[A]]
; CHECK-NOT: __yai_
; CHECK: ret

define void @phi_tile(i1 %c, ptr %p) {
entry:
  %a = call <1024 x i8> @__yai_tile_create()
  %b = call <1024 x i8> @__yai_tile_create()
  br i1 %c, label %then, label %else
then:
  br label %merge
else:
  br label %merge
merge:
  %t = phi <1024 x i8> [ %a, %then ], [ %b, %else ]
  call void @__yai_tile_load(<1024 x i8> %t)
  ret void
}

declare <1024 x i8> @__yai_tile_create()
declare void @__yai_tile_load(<1024 x i8>)
