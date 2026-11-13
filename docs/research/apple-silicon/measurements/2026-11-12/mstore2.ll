target triple = "arm64-apple-macos26.0"

; current approach: generic masked store
define void @masked(ptr %p, <16 x i8> %v, <16 x i1> %m) {
  call void @llvm.masked.store.v16i8.p0(<16 x i8> %v, ptr %p, i32 1, <16 x i1> %m)
  ret void
}

; merge-store: read 16B at aligned base, select bytes, write back
define void @merge(ptr %base, <16 x i8> %v, <16 x i1> %m) {
  %old = load <16 x i8>, ptr %base, align 1
  %new = select <16 x i1> %m, <16 x i8> %v, <16 x i8> %old
  store <16 x i8> %new, ptr %base, align 1
  ret void
}

declare void @llvm.masked.store.v16i8.p0(<16 x i8>, ptr, i32, <16 x i1>)
