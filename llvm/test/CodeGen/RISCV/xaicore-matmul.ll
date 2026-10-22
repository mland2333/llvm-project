; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; 两个 Tile 做矩阵乘得到 Acc（非转置 mma.nn）。
;
;   lhs (t0) x rhs (t1) -> acc (a0)   // 具体编号由 RA 决定
;
; 同时验证 Acc 拿到了自己的寄存器类/配置（am/ac）以及 astore。

; CHECK-LABEL: tile_matmul:
; CHECK: cfg.setx tm{{[0-9]+}}, base_addr,
; CHECK: cfg.setx tc{{[0-9]+}}, dtype,
; CHECK: tload {{t[0-9]+}}
; CHECK: cfg.setx tm{{[0-9]+}}, base_addr,
; CHECK: cfg.setx tc{{[0-9]+}}, dtype,
; CHECK: tload {{t[0-9]+}}
; CHECK: cfg.setx am{{[0-9]+}}, base_addr,
; CHECK: cfg.setx am{{[0-9]+}}, row_stride_bytes,
; CHECK: cfg.setx am{{[0-9]+}}, col_stride_bytes,
; CHECK: cfg.setx ac{{[0-9]+}}, dtype,
; CHECK: cfg.setx ac{{[0-9]+}}, rows,
; CHECK: cfg.setx ac{{[0-9]+}}, cols,
; CHECK: mma.nn.zero.i8.i32 [[AC:a[0-9]+]], [[TA:t[0-9]+]], [[TB:t[0-9]+]]
; CHECK: astore [[AC]]
; CHECK-NOT: __yai_
; CHECK: ret

define void @tile_matmul(ptr %lhs_ptr, ptr %rhs_ptr, ptr %out_ptr,
                         i32 signext %rows, i32 signext %cols) {
entry:
  %lhs = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_set_data(<1024 x i8> %lhs, ptr %lhs_ptr)
  call void @__yai_tile_set_strides(<1024 x i8> %lhs, i32 128, i32 1)
  call void @__yai_tile_set_dtype(<1024 x i8> %lhs, i32 0)
  call void @__yai_tile_load(<1024 x i8> %lhs)

  %rhs = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_set_data(<1024 x i8> %rhs, ptr %rhs_ptr)
  call void @__yai_tile_set_strides(<1024 x i8> %rhs, i32 128, i32 1)
  call void @__yai_tile_set_dtype(<1024 x i8> %rhs, i32 0)
  call void @__yai_tile_load(<1024 x i8> %rhs)

  %acc = call <1024 x i32> @__yai_acc_create()
  call void @__yai_acc_set_data(<1024 x i32> %acc, ptr %out_ptr)
  call void @__yai_acc_set_strides(<1024 x i32> %acc, i32 128, i32 4)
  call void @__yai_acc_set_dtype(<1024 x i32> %acc, i32 1)
  %r = trunc i32 %rows to i8
  %c = trunc i32 %cols to i8
  call void @__yai_acc_set_valid_zone_pair(<1024 x i32> %acc, i8 %r, i8 %c)

  call void @__yai_tile_matmul(<1024 x i8> %lhs, <1024 x i8> %rhs,
                               <1024 x i32> %acc, i1 true, i1 false, i1 false)
  call void @__yai_acc_store(<1024 x i32> %acc)
  ret void
}

; zero=false 表示累加：acc += lhs * rhs，走 mma.nn.acc。
; CHECK-LABEL: matmul_acc:
; CHECK: mma.nn.acc.i8.i32
; CHECK: astore
define void @matmul_acc(ptr %p, ptr %q, ptr %out) {
entry:
  %lhs = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_load(<1024 x i8> %lhs)
  %rhs = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_load(<1024 x i8> %rhs)
  %acc = call <1024 x i32> @__yai_acc_create()
  call void @__yai_tile_matmul(<1024 x i8> %lhs, <1024 x i8> %rhs,
                               <1024 x i32> %acc, i1 false, i1 false, i1 false)
  call void @__yai_acc_store(<1024 x i32> %acc)
  ret void
}

declare <1024 x i8> @__yai_tile_create()
declare <1024 x i32> @__yai_acc_create()
declare void @__yai_tile_set_data(<1024 x i8>, ptr)
declare void @__yai_tile_set_strides(<1024 x i8>, i32, i32)
declare void @__yai_tile_set_dtype(<1024 x i8>, i32)
declare void @__yai_tile_load(<1024 x i8>)
declare void @__yai_acc_set_data(<1024 x i32>, ptr)
declare void @__yai_acc_set_strides(<1024 x i32>, i32, i32)
declare void @__yai_acc_set_dtype(<1024 x i32>, i32)
declare void @__yai_acc_set_valid_zone_pair(<1024 x i32>, i8, i8)
declare void @__yai_acc_store(<1024 x i32>)
declare void @__yai_tile_matmul(<1024 x i8>, <1024 x i8>, <1024 x i32>, i1, i1, i1)
