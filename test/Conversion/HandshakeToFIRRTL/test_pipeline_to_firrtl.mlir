// RUN: circt-opt -lower-handshake-to-firrtl %s | FileCheck %s

// CHECK-LABEL: firrtl.module @staticlogic.pipeline_0(
// CHECK-SAME:  %arg0: !firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>, %arg1: !firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>, %arg2: !firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>, %arg3: !firrtl.bundle<valid: flip<uint<1>>, ready: uint<1>, data: flip<uint<32>>>, %clock: !firrtl.clock, %reset: !firrtl.uint<1>) {
// CHECK:         %0 = firrtl.subfield %arg0("valid") : (!firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>) -> !firrtl.uint<1>
// CHECK:         %1 = firrtl.subfield %arg0("ready") : (!firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>) -> !firrtl.flip<uint<1>>
// CHECK:         %2 = firrtl.subfield %arg0("data") : (!firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>) -> !firrtl.uint<32>
// CHECK:         %3 = firrtl.subfield %arg1("valid") : (!firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>) -> !firrtl.uint<1>
// CHECK:         %4 = firrtl.subfield %arg1("ready") : (!firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>) -> !firrtl.flip<uint<1>>
// CHECK:         %5 = firrtl.subfield %arg1("data") : (!firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>) -> !firrtl.uint<32>
// CHECK:         %6 = firrtl.subfield %arg2("valid") : (!firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>) -> !firrtl.uint<1>
// CHECK:         %7 = firrtl.subfield %arg2("ready") : (!firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>) -> !firrtl.flip<uint<1>>
// CHECK:         %8 = firrtl.subfield %arg2("data") : (!firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>) -> !firrtl.uint<32>
// CHECK:         %9 = firrtl.subfield %arg3("valid") : (!firrtl.bundle<valid: flip<uint<1>>, ready: uint<1>, data: flip<uint<32>>>) -> !firrtl.flip<uint<1>>
// CHECK:         %10 = firrtl.subfield %arg3("ready") : (!firrtl.bundle<valid: flip<uint<1>>, ready: uint<1>, data: flip<uint<32>>>) -> !firrtl.uint<1>
// CHECK:         %11 = firrtl.subfield %arg3("data") : (!firrtl.bundle<valid: flip<uint<1>>, ready: uint<1>, data: flip<uint<32>>>) -> !firrtl.flip<uint<32>>

module {
  // CHECK-LABEL: firrtl.module @ops(
  // CHECK-SAME:  %arg0: !firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>, %arg1: !firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>, %arg2: !firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>, %arg3: !firrtl.bundle<valid: uint<1>, ready: flip<uint<1>>>, %arg4: !firrtl.bundle<valid: flip<uint<1>>, ready: uint<1>, data: flip<uint<32>>>, %arg5: !firrtl.bundle<valid: flip<uint<1>>, ready: uint<1>>, %clock: !firrtl.clock, %reset: !firrtl.uint<1>) {
  handshake.func @ops(%arg0: i32, %arg1: i32, %arg2: i32, %arg3: none, ...) -> (i32, none) {
    
    // CHECK: %0 = firrtl.instance @staticlogic.pipeline_0 {name = ""} : !firrtl.bundle<arg0: bundle<valid: flip<uint<1>>, ready: uint<1>, data: flip<uint<32>>>, arg1: bundle<valid: flip<uint<1>>, ready: uint<1>, data: flip<uint<32>>>, arg2: bundle<valid: flip<uint<1>>, ready: uint<1>, data: flip<uint<32>>>, arg3: bundle<valid: uint<1>, ready: flip<uint<1>>, data: uint<32>>, arg4: flip<clock>, arg5: flip<uint<1>>>
    %0 = "staticlogic.pipeline"(%arg0, %arg1, %arg2) ( {
    ^bb0(%arg4: i32, %arg5: i32, %arg6: i32):  // no predecessors
      %1 = addi %arg4, %arg5 : i32
      br ^bb1
    ^bb1:  // pred: ^bb0
      %2 = addi %arg4, %1 : i32
      %3 = addi %arg6, %1 : i32
      br ^bb2
    ^bb2:  // pred: ^bb1
      %4 = addi %2, %3 : i32
      "staticlogic.return"(%4) : (i32) -> ()
    }) : (i32, i32, i32) -> i32
    handshake.return %0, %arg3 : i32, none
  }
}