target triple = "arm64-apple-macos26.0"

; 1) cmpwi + bt only: ideal is cmp + b.lt
define i64 @cmp_branch(i64 %a, i64 %b, i64 %x, i64 %y) {
entry:
  %lt = icmp slt i64 %a, %b
  br i1 %lt, label %t, label %f
t:
  %r1 = add i64 %x, %y
  ret i64 %r1
f:
  %r2 = sub i64 %x, %y
  ret i64 %r2
}

; 2) full CR field (lt, gt, eq) live, branch on lt
define i64 @cmp_fullcr(i64 %a, i64 %b, i64 %x, i64 %y, ptr %out) {
entry:
  %lt = icmp slt i64 %a, %b
  %gt = icmp sgt i64 %a, %b
  %eq = icmp eq i64 %a, %b
  %z = zext i1 %lt to i8
  %o = zext i1 %gt to i8
  %p = zext i1 %eq to i8
  store i8 %z, ptr %out
  %o1 = getelementptr i8, ptr %out, i64 1
  store i8 %o, ptr %o1
  %p1 = getelementptr i8, ptr %out, i64 2
  store i8 %p, ptr %p1
  br i1 %lt, label %t, label %f
t:
  ret i64 %x
f:
  ret i64 %y
}

; 3) SPEC-like pattern: compare against zero, both branch and one bit stored
define i64 @cmp_zero_branch(i64 %a, i64 %x, ptr %out) {
entry:
  %lt = icmp slt i64 %a, 0
  %z = zext i1 %lt to i8
  store i8 %z, ptr %out
  br i1 %lt, label %t, label %f
t:
  ret i64 1
f:
  ret i64 0
}
