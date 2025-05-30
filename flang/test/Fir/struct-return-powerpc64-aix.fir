// Test powerpc64 ABI rewrite of struct returned by value (BIND(C), VALUE derived types).
//
// RUN: fir-opt --target-rewrite="target=powerpc64-ibm-aix" %s | FileCheck %s

module attributes {fir.defaultkind = "a1c4d8i4l4r4", fir.kindmap = "", llvm.data_layout = "E-m:a-Fi64-i64:64-i128:128-n32:64-S128-v256:256:256-v512:512:512", llvm.target_triple = "powerpc64-ibm-aix7.2.0.0"} {

// character type
func.func private @test_t1() -> !fir.type<t1<{c:!fir.char<1>}>>
//CHECK-LABEL: func.func private @test_t1(!fir.ref<!fir.type<t1<{c:!fir.char<1>}>>> {llvm.align = 8 : i32, llvm.sret = !fir.type<t1<{c:!fir.char<1>}>>})
func.func @test_call_t1(%arg0 : !fir.ref<!fir.type<t1<{c:!fir.char<1>}>>>) {
//CHECK-LABEL: func.func @test_call_t1(
//CHECK-SAME:    %[[ARG0:.*]]: !fir.ref<!fir.type<t1<{c:!fir.char<1>}>>>)
  %out = fir.call @test_t1() : () -> !fir.type<t1<{c:!fir.char<1>}>>
  fir.store %out to %arg0 : !fir.ref<!fir.type<t1<{c:!fir.char<1>}>>>
  return
  //CHECK: %[[STCK:.*]] = llvm.intr.stacksave : !llvm.ptr
  //CHECK: %[[ARG:.*]] = fir.alloca !fir.type<t1<{c:!fir.char<1>}>>
  //CHECK: fir.call @test_t1(%[[ARG]]) : (!fir.ref<!fir.type<t1<{c:!fir.char<1>}>>> {llvm.align = 8 : i32, llvm.sret = !fir.type<t1<{c:!fir.char<1>}>>}) -> ()
  //CHECK: %[[CVT:.*]] = fir.convert %[[ARG]] : (!fir.ref<!fir.type<t1<{c:!fir.char<1>}>>>) -> !fir.ref<!fir.type<t1<{c:!fir.char<1>}>>>
  //CHECK: %[[LD:.*]] = fir.load %[[CVT]] : !fir.ref<!fir.type<t1<{c:!fir.char<1>}>>>
  //CHECK: llvm.intr.stackrestore %[[STCK]] : !llvm.ptr
  //CHECK: fir.store %[[LD]] to %[[ARG0]] : !fir.ref<!fir.type<t1<{c:!fir.char<1>}>>>
  //CHECK: return
}

// integer type
func.func private @test_t2() -> !fir.type<t2<{i:i32}>>
//CHECK-LABEL: func.func private @test_t2(!fir.ref<!fir.type<t2<{i:i32}>>> {llvm.align = 8 : i32, llvm.sret = !fir.type<t2<{i:i32}>>})
func.func @test_call_t2(%arg0 : !fir.ref<!fir.type<t2<{i:i32}>>>) {
//CHECK-LABEL: func.func @test_call_t2(
//CHECK-SAME:    %[[ARG0:.*]]: !fir.ref<!fir.type<t2<{i:i32}>>>)
  %out = fir.call @test_t2() : () -> !fir.type<t2<{i:i32}>>
  fir.store %out to %arg0 : !fir.ref<!fir.type<t2<{i:i32}>>>
  return
  //CHECK: %[[STCK:.*]] = llvm.intr.stacksave : !llvm.ptr
  //CHECK: %[[ARG:.*]] = fir.alloca !fir.type<t2<{i:i32}>>
  //CHECK: fir.call @test_t2(%[[ARG]]) : (!fir.ref<!fir.type<t2<{i:i32}>>> {llvm.align = 8 : i32, llvm.sret = !fir.type<t2<{i:i32}>>}) -> ()
  //CHECK: %[[CVT:.*]] = fir.convert %[[ARG]] : (!fir.ref<!fir.type<t2<{i:i32}>>>) -> !fir.ref<!fir.type<t2<{i:i32}>>>
  //CHECK: %[[LD:.*]] = fir.load %[[CVT]] : !fir.ref<!fir.type<t2<{i:i32}>>>
  //CHECK: llvm.intr.stackrestore %[[STCK]] : !llvm.ptr
  //CHECK: fir.store %[[LD]] to %[[ARG0]] : !fir.ref<!fir.type<t2<{i:i32}>>>
  //CHECK: return
}

// real type (scalar)
func.func private @test_t3() -> !fir.type<t3<{r1:f32,r2:f32,r3:f32}>>
//CHECK-LABEL: func.func private @test_t3(!fir.ref<!fir.type<t3<{r1:f32,r2:f32,r3:f32}>>> {llvm.align = 8 : i32, llvm.sret = !fir.type<t3<{r1:f32,r2:f32,r3:f32}>>})
func.func @test_call_t3(%arg0 : !fir.ref<!fir.type<t3<{r1:f32,r2:f32,r3:f32}>>>) {
//CHECK-LABEL: func.func @test_call_t3(
//CHECK-SAME:    %[[ARG0:.*]]: !fir.ref<!fir.type<t3<{r1:f32,r2:f32,r3:f32}>>>)
  %out = fir.call @test_t3() : () -> !fir.type<t3<{r1:f32,r2:f32,r3:f32}>>
  fir.store %out to %arg0 : !fir.ref<!fir.type<t3<{r1:f32,r2:f32,r3:f32}>>>
  return
  //CHECK: %[[STCK:.*]] = llvm.intr.stacksave : !llvm.ptr
  //CHECK: %[[ARG:.*]] = fir.alloca !fir.type<t3<{r1:f32,r2:f32,r3:f32}>>
  //CHECK: fir.call @test_t3(%[[ARG]]) : (!fir.ref<!fir.type<t3<{r1:f32,r2:f32,r3:f32}>>> {llvm.align = 8 : i32, llvm.sret = !fir.type<t3<{r1:f32,r2:f32,r3:f32}>>}) -> ()
  //CHECK: %[[CVT:.*]] = fir.convert %[[ARG]] : (!fir.ref<!fir.type<t3<{r1:f32,r2:f32,r3:f32}>>>) -> !fir.ref<!fir.type<t3<{r1:f32,r2:f32,r3:f32}>>>
  //CHECK: %[[LD:.*]] = fir.load %[[CVT]] : !fir.ref<!fir.type<t3<{r1:f32,r2:f32,r3:f32}>>>
  //CHECK: llvm.intr.stackrestore %[[STCK]] : !llvm.ptr
  //CHECK: fir.store %[[LD]] to %[[ARG0]] : !fir.ref<!fir.type<t3<{r1:f32,r2:f32,r3:f32}>>>
  //CHECK: return
}

// real type (array)
func.func private @test_t4() -> !fir.type<t4<{r:!fir.array<8xf32>}>>
//CHECK-LABEL: func.func private @test_t4(!fir.ref<!fir.type<t4<{r:!fir.array<8xf32>}>>> {llvm.align = 8 : i32, llvm.sret = !fir.type<t4<{r:!fir.array<8xf32>}>>})
func.func @test_call_t4(%arg0 : !fir.ref<!fir.type<t4<{r:!fir.array<8xf32>}>>>) {
//CHECK-LABEL: func.func @test_call_t4(
//CHECK-SAME:    %[[ARG0:.*]]: !fir.ref<!fir.type<t4<{r:!fir.array<8xf32>}>>>)
  %out = fir.call @test_t4() : () -> !fir.type<t4<{r:!fir.array<8xf32>}>>
  fir.store %out to %arg0 : !fir.ref<!fir.type<t4<{r:!fir.array<8xf32>}>>>
  return
  //CHECK: %[[STCK:.*]] = llvm.intr.stacksave : !llvm.ptr
  //CHECK: %[[ARG:.*]] = fir.alloca !fir.type<t4<{r:!fir.array<8xf32>}>>
  //CHECK: fir.call @test_t4(%[[ARG]]) : (!fir.ref<!fir.type<t4<{r:!fir.array<8xf32>}>>> {llvm.align = 8 : i32, llvm.sret = !fir.type<t4<{r:!fir.array<8xf32>}>>}) -> ()
  //CHECK: %[[CVT:.*]] = fir.convert %[[ARG]] : (!fir.ref<!fir.type<t4<{r:!fir.array<8xf32>}>>>) -> !fir.ref<!fir.type<t4<{r:!fir.array<8xf32>}>>>
  //CHECK: %[[LD:.*]] = fir.load %[[CVT]] : !fir.ref<!fir.type<t4<{r:!fir.array<8xf32>}>>>
  //CHECK: llvm.intr.stackrestore %[[STCK]] : !llvm.ptr
  //CHECK: fir.store %[[LD]] to %[[ARG0]] : !fir.ref<!fir.type<t4<{r:!fir.array<8xf32>}>>>
  //CHECK: return
}

// mixed types
func.func private @test_t5() -> !fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>
//CHECK-LABEL: func.func private @test_t5(!fir.ref<!fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>> {llvm.align = 8 : i32, llvm.sret = !fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>})
func.func @test_call_t5(%arg0 : !fir.ref<!fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>>) {
//CHECK-LABEL: func.func @test_call_t5(
//CHECK-SAME:    %[[ARG0:.*]]: !fir.ref<!fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>>)
  %out = fir.call @test_t5() : () -> !fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>
  fir.store %out to %arg0 : !fir.ref<!fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>>
  return
  //CHECK: %[[STCK:.*]] = llvm.intr.stacksave : !llvm.ptr
  //CHECK: %[[ARG:.*]] = fir.alloca !fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>
  //CHECK: fir.call @test_t5(%[[ARG]]) : (!fir.ref<!fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>> {llvm.align = 8 : i32, llvm.sret = !fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>}) -> ()
  //CHECK: %[[CVT:.*]] = fir.convert %[[ARG]] : (!fir.ref<!fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>>) -> !fir.ref<!fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>>
  //CHECK: %[[LD:.*]] = fir.load %[[CVT]] : !fir.ref<!fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>>
  //CHECK: llvm.intr.stackrestore %[[STCK]] : !llvm.ptr
  //CHECK: fir.store %[[LD]] to %[[ARG0]] : !fir.ref<!fir.type<t5<{c:!fir.char<1>,r:f32,i:i64}>>>
  //CHECK: return
}

}
