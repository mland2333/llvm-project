; RUN: llc -mtriple=riscv64 -mattr=+xaicore < %s | FileCheck %s

; Tile 是 LLVM 的 SSA 值（<1024 x i8> 载体），不再是整数句柄。后端把它映射
; 到 XTT 寄存器类（t0..t15），由寄存器分配器决定具体编号；汇编里的 tN/tcN/tmN
; 因此是分配结果，而不是写死的 t0。
;
; 这个测试对应 tile_api_llvm_ds/tile_fixed_mix.cpp 的 -O1 IR。测试不带
; target-features 属性，feature 由 -mattr=+xaicore 提供（llc 会把命令行
; 特性追加到函数的 target-features 上）。

; CHECK-LABEL: tile_fixed_mix:
; CHECK: cfg.setx {{.*}}tm{{[0-9]+}}, base_addr,
; CHECK: cfg.setx {{.*}}tm{{[0-9]+}}, row_stride_bytes,
; CHECK: cfg.setx {{.*}}tm{{[0-9]+}}, col_stride_bytes,
; CHECK: cfg.setx {{.*}}tc{{[0-9]+}}, dtype,
; CHECK: cfg.setx {{.*}}tc{{[0-9]+}}, rows,
; CHECK: cfg.setx {{.*}}tc{{[0-9]+}}, cols,
; CHECK: tload {{t[0-9]+}}
; CHECK: cfg.get {{.*}}tc{{[0-9]+}}, rows
; CHECK: addiw
; CHECK: bgeu
; CHECK: bge
; CHECK: tstore {{t[0-9]+}}
; CHECK-NOT: __yai_
; CHECK: ret

; 缺少 +xaicore 时由 clang 在前端拒绝（"needs target feature xaicore"）；
; 这个测试只覆盖打开扩展后的后端 lowering。

define dso_local i64 @tile_fixed_mix(ptr %ptr, i32 signext %rows, i32 signext %max_blocks) {
entry:
  %tile = call <1024 x i8> @__yai_tile_create()
  call void @__yai_tile_set_data(<1024 x i8> %tile, ptr %ptr)
  call void @__yai_tile_set_strides(<1024 x i8> %tile, i32 128, i32 1)
  call void @__yai_tile_set_dtype(<1024 x i8> %tile, i32 0)
  br label %while.cond

while.cond:                                       ; preds = %if.end, %entry
  %block.0 = phi i32 [ 0, %entry ], [ %block.1, %if.end ]
  %row.0 = phi i32 [ 0, %entry ], [ %row.1, %if.end ]
  %checksum.0 = phi i64 [ 0, %entry ], [ %add6, %if.end ]
  %cmp = icmp slt i32 %row.0, %rows
  %cmp1 = icmp slt i32 %block.0, %max_blocks
  %1 = select i1 %cmp, i1 %cmp1, i1 false
  br i1 %1, label %if.end, label %while.end

if.end:                                           ; preds = %while.cond
  %sub = sub nsw i32 %rows, %row.0
  %cond = call i32 @llvm.smin.i32(i32 %sub, i32 32)
  %conv = trunc nuw i32 %cond to i8
  call void @__yai_tile_set_valid_zone_pair(<1024 x i8> %tile, i8 %conv, i8 32)
  call void @__yai_tile_load(<1024 x i8> %tile)
  %2 = call i64 @__yai_tile_get_valid_zone(<1024 x i8> %tile, i64 0)
  %conv4 = and i64 %2, 255
  %mul = mul nuw nsw i64 %conv4, 3
  %conv5 = sext i32 %row.0 to i64
  %add = add i64 %checksum.0, %conv5
  %add6 = add i64 %add, %mul
  %cmp7 = icmp ult i64 %add6, 4097
  %add10 = add nsw i32 %row.0, 32
  %inc = zext i1 %cmp7 to i32
  %block.1 = add nuw nsw i32 %block.0, %inc
  %row.1 = select i1 %cmp7, i32 %add10, i32 %row.0
  br i1 %cmp7, label %while.cond, label %while.end

while.end:                                        ; preds = %if.end, %while.cond
  %checksum.2 = phi i64 [ %add6, %if.end ], [ %checksum.0, %while.cond ]
  call void @__yai_tile_store(<1024 x i8> %tile)
  ret i64 %checksum.2
}

declare <1024 x i8> @__yai_tile_create()
declare void @__yai_tile_set_data(<1024 x i8>, ptr)
declare void @__yai_tile_set_strides(<1024 x i8>, i32, i32)
declare void @__yai_tile_set_dtype(<1024 x i8>, i32)
declare void @__yai_tile_set_valid_zone_pair(<1024 x i8>, i8, i8)
declare void @__yai_tile_load(<1024 x i8>)
declare i64 @__yai_tile_get_valid_zone(<1024 x i8>, i64)
declare void @__yai_tile_store(<1024 x i8>)
declare i32 @llvm.smin.i32(i32, i32)
