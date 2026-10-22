; RUN: not llc -mtriple=riscv64 -mattr=+xaicore < %s 2>&1 | FileCheck %s

; ISA 没有条件移动 Tile 寄存器的指令，所以 select/三目必须先展开成
; 分支 + PHI。后端对 select 给出明确诊断，而不是 isel 的 "Cannot select"。
;
; CHECK: YAI tile value cannot be conditionally selected between two registers

define void @select_tile(i1 %c, ptr %p) {
entry:
  %a = call <1024 x i8> @__yai_tile_create()
  %b = call <1024 x i8> @__yai_tile_create()
  %t = select i1 %c, <1024 x i8> %a, <1024 x i8> %b
  call void @__yai_tile_load(<1024 x i8> %t)
  ret void
}

declare <1024 x i8> @__yai_tile_create()
declare void @__yai_tile_load(<1024 x i8>)
