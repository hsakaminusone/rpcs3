#include <llvm/ADT/SmallVector.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetOptions.h>
#include <cstdio>
#include <cstring>

using namespace llvm;

struct LogMM : SectionMemoryManager {
  uint8_t* stack_sizes = nullptr; size_t stack_sizes_len = 0;
  uint8_t* allocateDataSection(uintptr_t Size, unsigned Alignment, unsigned SectionID,
                               StringRef SectionName, bool IsReadOnly) override {
    fprintf(stderr, "DATA   name='%s' size=%zu align=%u ro=%d\n", SectionName.str().c_str(), (size_t)Size, Alignment, (int)IsReadOnly);
    fflush(stderr);
    uint8_t* p = SectionMemoryManager::allocateDataSection(Size, Alignment, SectionID, SectionName, IsReadOnly);
    if (SectionName == ".stack_sizes") { stack_sizes = p; stack_sizes_len = Size; }
    return p;
  }
  uint8_t* allocateCodeSection(uintptr_t Size, unsigned Alignment, unsigned SectionID,
                               StringRef SectionName) override {
    fprintf(stderr, "CODE   name='%s' size=%zu align=%u\n", SectionName.str().c_str(), (size_t)Size, Alignment);
    fflush(stderr);
    return SectionMemoryManager::allocateCodeSection(Size, Alignment, SectionID, SectionName);
  }
};

int main() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  LLVMLinkInMCJIT();

  LLVMContext ctx;
  auto mod = std::make_unique<Module>("t", ctx);
  mod->setTargetTriple(Triple("aarch64-unknown-linux-android"));
  mod->setDataLayout("e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128");

  FunctionType* ft = FunctionType::get(Type::getInt64Ty(ctx), {Type::getInt64Ty(ctx)}, false);
  Function* f = Function::Create(ft, GlobalValue::ExternalLinkage, "big", mod.get());
  BasicBlock* bb = BasicBlock::Create(ctx, "entry", f);
  IRBuilder<> b(bb);
  AllocaInst* a = b.CreateAlloca(Type::getInt32Ty(ctx), b.getInt32(300));
  Value* g = b.CreateGEP(Type::getInt32Ty(ctx), a, b.CreateAnd(f->arg_begin(), b.getInt64(255)));
  Value* v = b.CreateLoad(Type::getInt32Ty(ctx), g);
  b.CreateRet(b.CreateSExt(v, b.getInt64Ty()));

  std::string err;
  TargetOptions opts;
  opts.EmitStackSizeSection = true;

  auto mm = std::make_unique<LogMM>(); LogMM* mmraw = mm.get();
  ExecutionEngine* ee = EngineBuilder(std::move(mod))
      .setErrorStr(&err)
      .setEngineKind(EngineKind::JIT)
      .setMCJITMemoryManager(std::move(mm))
      .setOptLevel(CodeGenOptLevel::Aggressive)
      .setRelocationModel(Reloc::Model::PIC_)
      .setTargetOptions(opts)
      .create();
  if (!ee) { fprintf(stderr, "engine fail: %s\n", err.c_str()); return 1; }
  ee->setProcessAllSections(true);
  fprintf(stderr, "engine ok; generating\n");
  fflush(stderr);
  LogMM* mm2 = mmraw;
  ee->finalizeObject();
  fprintf(stderr, "finalized\n");
  if (mm2->stack_sizes) {
    fprintf(stderr, "stack_sizes payload (%zu bytes):", mm2->stack_sizes_len);
    for (size_t i=0;i<mm2->stack_sizes_len;i++) fprintf(stderr, " %02x", mm2->stack_sizes[i]);
    fprintf(stderr, "\n");
    size_t i=0; int rec=0;
    while (i + 8 < mm2->stack_sizes_len && rec < 4) {
      uint64_t addr; memcpy(&addr, mm2->stack_sizes+i, 8); i += 8;
      uint64_t val=0; int shift=0; bool cont;
      do { uint8_t b=mm2->stack_sizes[i++]; val |= (uint64_t)(b&0x7f)<<shift; shift+=7; cont=b&0x80; } while (cont);
      fprintf(stderr, "  record: addr=0x%llx frame=%llu\n", (unsigned long long)addr, (unsigned long long)val);
      rec++;
    }
  } else {
    fprintf(stderr, "stack_sizes section NOT captured\n");
  }
  return 0;
}
