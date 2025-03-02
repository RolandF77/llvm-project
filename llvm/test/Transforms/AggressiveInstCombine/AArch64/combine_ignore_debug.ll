; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 3
; RUN: opt -mtriple aarch64 -aggressive-instcombine-max-scan-instrs=1 -passes="aggressive-instcombine" -S < %s | FileCheck %s -check-prefix DBG
; RUN: opt -strip-debug -mtriple aarch64 -aggressive-instcombine-max-scan-instrs=1 -passes="aggressive-instcombine" -S < %s | FileCheck %s -check-prefix NODBG

; The DBG and NODBG cases should be the same. I.e. we should optimize the DBG
; case too even if there is a dbg.value.

target datalayout = "E"

%s = type { i16, i16 }

@e = global %s zeroinitializer, align 1
@l = global %s zeroinitializer, align 1

define void @test() {
; DBG-LABEL: define void @test() {
; DBG-NEXT:  entry:
; DBG-NEXT:    [[L1:%.*]] = load i32, ptr @e, align 1
; DBG-NEXT:      #dbg_value(i32 poison, [[META3:![0-9]+]], !DIExpression(), [[META5:![0-9]+]])
; DBG-NEXT:    store i32 [[L1]], ptr @l, align 1
; DBG-NEXT:    ret void
;
; NODBG-LABEL: define void @test() {
; NODBG-NEXT:  entry:
; NODBG-NEXT:    [[L1:%.*]] = load i32, ptr @e, align 1
; NODBG-NEXT:    store i32 [[L1]], ptr @l, align 1
; NODBG-NEXT:    ret void
;
entry:
  %l1 = load i16, ptr @e, align 1
  call void @llvm.dbg.value(metadata i32 poison, metadata !3, metadata !DIExpression()), !dbg !5
  %l2 = load i16, ptr getelementptr inbounds (%s, ptr @e, i16 0, i32 1), align 1
  %e2 = zext i16 %l2 to i32
  %e1 = zext i16 %l1 to i32
  %s1 = shl nuw i32 %e1, 16
  %o1 = or i32 %s1, %e2
  store i32 %o1, ptr @l, align 1
  ret void
}

declare void @llvm.dbg.value(metadata, metadata, metadata)

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1)
!1 = !DIFile(filename: "foo.c", directory: "/")
!2 = !{i32 2, !"Debug Info Version", i32 3}
!3 = !DILocalVariable(scope: !4)
!4 = distinct !DISubprogram(unit: !0)
!5 = !DILocation(scope: !4)
