target triple = "aarch64-unknown-linux-android"

declare void @g(ptr)

define ghccc void @f(ptr %p) {
entry:
  %a = alloca [300 x i32], align 16
  %q0 = getelementptr inbounds [300 x i32], ptr %a, i64 0, i64 0
  %q1 = getelementptr inbounds [300 x i32], ptr %a, i64 0, i64 1
  %q2 = getelementptr inbounds [300 x i32], ptr %a, i64 0, i64 2
  %q3 = getelementptr inbounds [300 x i32], ptr %a, i64 0, i64 3
  %q4 = getelementptr inbounds [300 x i32], ptr %a, i64 0, i64 4
  %q5 = getelementptr inbounds [300 x i32], ptr %a, i64 0, i64 5
  %q6 = getelementptr inbounds [300 x i32], ptr %a, i64 0, i64 6
  %q7 = getelementptr inbounds [300 x i32], ptr %a, i64 0, i64 7
  store volatile i32 1, ptr %q0
  store volatile i32 2, ptr %q1
  store volatile i32 3, ptr %q2
  store volatile i32 4, ptr %q3
  store volatile i32 5, ptr %q4
  store volatile i32 6, ptr %q5
  store volatile i32 7, ptr %q6
  store volatile i32 8, ptr %q7
  call void @g(ptr %a)
  ret void
}

define ghccc void @h(ptr %p) {
entry:
  %v0 = load volatile i32, ptr %p
  %v1 = load volatile i32, ptr %p
  %v2 = load volatile i32, ptr %p
  %v3 = load volatile i32, ptr %p
  %v4 = load volatile i32, ptr %p
  %v5 = load volatile i32, ptr %p
  %v6 = load volatile i32, ptr %p
  %v7 = load volatile i32, ptr %p
  %v8 = load volatile i32, ptr %p
  %v9 = load volatile i32, ptr %p
  %v10 = load volatile i32, ptr %p
  %v11 = load volatile i32, ptr %p
  %v12 = load volatile i32, ptr %p
  %v13 = load volatile i32, ptr %p
  %v14 = load volatile i32, ptr %p
  %v15 = load volatile i32, ptr %p
  %v16 = load volatile i32, ptr %p
  %v17 = load volatile i32, ptr %p
  %v18 = load volatile i32, ptr %p
  %v19 = load volatile i32, ptr %p
  call void @g(ptr %p)
  ret void
}
