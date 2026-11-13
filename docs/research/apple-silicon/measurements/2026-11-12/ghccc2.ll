target triple = "aarch64-unknown-linux-android"
declare void @g(ptr)
define ghccc void @big(ptr %p) {
entry:
  %a = alloca [3000 x i32], align 16
  %q0 = getelementptr inbounds [3000 x i32], ptr %a, i64 0, i64 0
  %q1 = getelementptr inbounds [3000 x i32], ptr %a, i64 0, i64 2999
  store volatile i32 1, ptr %q0
  store volatile i32 2, ptr %q1
  call void @g(ptr %a)
  ret void
}
