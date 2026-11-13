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

using namespace llvm;

struct LogMM : SectionMemoryManager {
  uint8_t* allocateDataSection(uintptr_t Size, unsigned Alignment, unsigned SectionID,
                               StringRef SectionName, bool IsReadOnly) override {
    fprintf(stderr, "DATA   name='%s' size=%zu align=%u ro=%d\n", SectionName.str().c_str(), (size_t)Size, Alignment, (int)IsReadOnly);
    fflush(stderr);
    return SectionMemoryManager::allocateDataSection(Size, Alignment, SectionID, SectionName, IsReadOnly);
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

  auto mm = std::make_unique<LogMM>();
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
  ee->finalizeObject();
  fprintf(stderr, "finalized\n");
  return 0;
}
