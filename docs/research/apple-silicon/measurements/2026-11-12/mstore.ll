target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
target triple = "arm64-apple-macos26.0"

declare void @llvm.masked.store.v16i8.p0(<16 x i8>, ptr, i32 immarg, <16 x i1>)

define void @st16(ptr %p, <16 x i8> %v, <16 x i1> %m) {
  call void @llvm.masked.store.v16i8.p0(<16 x i8> %v, ptr %p, i32 1, <16 x i1> %m)
  ret void
}

define void @ld16(ptr %p, ptr %out, <16 x i1> %m) {
  %v = call <16 x i8> @llvm.masked.load.v16i8.p0(ptr %p, i32 1, <16 x i1> %m, <16 x i8> zeroinitializer)
  store <16 x i8> %v, ptr %out, align 1
  ret void
}
declare <16 x i8> @llvm.masked.load.v16i8.p0(ptr, i32 immarg, <16 x i1>, <16 x i8>)

define {i64, i64} @ldp16(ptr %p) {
  %v = load atomic i128, ptr %p acquire, align 16
  %lo = trunc i128 %v to i64
  %hi = lshr i128 %v, 64
  %hi64 = trunc i128 %hi to i64
  %a = insertvalue {i64,i64} undef, i64 %lo, 0
  %b = insertvalue {i64,i64} %a, i64 %hi64, 1
  ret {i64,i64} %b
}

define void @stp16(ptr %p, i64 %a, i64 %b) {
  %lo = zext i64 %a to i128
  %hi = zext i64 %b to i128
  %hish = shl i128 %hi, 64
  %v = or i128 %lo, %hish
  store atomic i128 %v, ptr %p release, align 16
  ret void
}

define {i64,i64} @casp(ptr %p, i64 %a0, i64 %a1, i64 %n0, i64 %n1) {
  %old = cmpxchg ptr %p, i128 0, i128 0 acq_rel monotonic
  ret {i64,i64} zeroinitializer
}
