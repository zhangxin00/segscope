#include "AutoInstrument/AutoInstrumentPass.h"
#include "AutoInstrument/IntMonBranchPass.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <cctype>

using namespace llvm;

namespace {

static cl::list<std::string> AidTargets(
    "aid-target",
    cl::desc("插桩目标指令类型（逗号分隔）：all,call,ret,br,load,store,mem,term"),
    cl::value_desc("types"),
    cl::CommaSeparated);

static cl::opt<std::string> AidAsm(
    "aid-asm",
    cl::desc("插入的内联汇编（x86 指令，默认为 nop）"),
    cl::init("nop"));

static cl::opt<bool> AidPrint(
    "aid-print",
    cl::desc("在插桩点插入 puts 打印，用于验证插桩是否触发"),
    cl::init(false));

static cl::opt<std::string> AidPrintPrefix(
    "aid-print-prefix",
    cl::desc("打印前缀（与函数名、指令类型拼接）"),
    cl::init("intr_detect"));

static cl::opt<bool> AidInsertAfter(
    "aid-insert-after",
    cl::desc("在目标指令之后插入（默认之前插入）"),
    cl::init(false));

static cl::opt<std::string> AidFuncAllow(
    "aid-func-allow",
    cl::desc("仅插桩匹配该正则的函数名"),
    cl::init(""));

static cl::opt<std::string> AidFuncDeny(
    "aid-func-deny",
    cl::desc("跳过匹配该正则的函数名"),
    cl::init(""));

static bool equalsInsensitive(StringRef A, StringRef B) {
  if (A.size() != B.size()) {
    return false;
  }
  for (size_t i = 0; i < A.size(); ++i) {
    unsigned char ca = static_cast<unsigned char>(A[i]);
    unsigned char cb = static_cast<unsigned char>(B[i]);
    if (std::tolower(ca) != std::tolower(cb)) {
      return false;
    }
  }
  return true;
}

bool hasTargetAll() {
  for (const auto &T : AidTargets) {
    if (equalsInsensitive(StringRef(T), "all")) {
      return true;
    }
  }
  return false;
}

bool hasTarget(StringRef Target) {
  if (AidTargets.empty()) {
    return equalsInsensitive(Target, "call");
  }
  for (const auto &T : AidTargets) {
    if (equalsInsensitive(StringRef(T), Target)) {
      return true;
    }
  }
  return false;
}

bool shouldSkipFunction(const Function &F,
                        const std::optional<Regex> &Allow,
                        const std::optional<Regex> &Deny) {
  if (F.isDeclaration()) {
    return true;
  }
  if (Allow && !Allow->match(F.getName())) {
    return true;
  }
  if (Deny && Deny->match(F.getName())) {
    return true;
  }
  return false;
}

bool shouldInstrumentInstruction(const Instruction &I) {
  if (isa<PHINode>(I) || isa<LandingPadInst>(I) || isa<DbgInfoIntrinsic>(I)) {
    return false;
  }

  if (hasTargetAll()) {
    return true;
  }

  if (hasTarget("call") && isa<CallBase>(I)) {
    return true;
  }
  if (hasTarget("ret") && isa<ReturnInst>(I)) {
    return true;
  }
  if (hasTarget("br") && (isa<BranchInst>(I) || isa<SwitchInst>(I) ||
                           isa<IndirectBrInst>(I))) {
    return true;
  }
  if (hasTarget("load") && isa<LoadInst>(I)) {
    return true;
  }
  if (hasTarget("store") && isa<StoreInst>(I)) {
    return true;
  }
  if (hasTarget("mem") && (isa<LoadInst>(I) || isa<StoreInst>(I) ||
                            isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I) ||
                            isa<MemIntrinsic>(I))) {
    return true;
  }
  if (hasTarget("term") && I.isTerminator()) {
    return true;
  }

  return false;
}

InlineAsm *getInlineAsm(Module &M) {
  if (AidAsm.empty()) {
    return nullptr;
  }
  LLVMContext &Ctx = M.getContext();
  FunctionType *FnTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  return InlineAsm::get(FnTy, AidAsm, "", true);
}

FunctionCallee getOrInsertPuts(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *I8PtrTy = PointerType::getUnqual(Type::getInt8Ty(Ctx));
  FunctionType *FnTy = FunctionType::get(Type::getInt32Ty(Ctx),
                                         {I8PtrTy},
                                         false);
  return M.getOrInsertFunction("puts", FnTy);
}

void emitPrint(IRBuilder<> &B, const Function &F, const Instruction &I,
               FunctionCallee Puts) {
  std::string Msg = AidPrintPrefix;
  Msg.append(": func=");
  Msg.append(F.getName().str());
  Msg.append(" op=");
  Msg.append(I.getOpcodeName());
  Value *MsgV = B.CreateGlobalStringPtr(Msg);
  B.CreateCall(Puts, {MsgV});
}

}  // namespace

namespace auto_instrument {

PreservedAnalyses AutoInstrumentPass::run(Module &M, ModuleAnalysisManager &MAM) {
  (void)MAM;

  std::optional<Regex> AllowRe;
  std::optional<Regex> DenyRe;

  if (!AidFuncAllow.empty()) {
    AllowRe.emplace(AidFuncAllow);
    std::string Error;
    if (!AllowRe->isValid(Error)) {
      errs() << "[AutoInstrumentPass] aid-func-allow 正则无效: " << Error << "\n";
      return PreservedAnalyses::all();
    }
  }

  if (!AidFuncDeny.empty()) {
    DenyRe.emplace(AidFuncDeny);
    std::string Error;
    if (!DenyRe->isValid(Error)) {
      errs() << "[AutoInstrumentPass] aid-func-deny 正则无效: " << Error << "\n";
      return PreservedAnalyses::all();
    }
  }

  InlineAsm *Asm = getInlineAsm(M);
  FunctionCallee Puts;
  if (AidPrint) {
    Puts = getOrInsertPuts(M);
  }

  for (Function &F : M) {
    if (shouldSkipFunction(F, AllowRe, DenyRe)) {
      continue;
    }

    for (BasicBlock &BB : F) {
      for (auto It = BB.begin(); It != BB.end();) {
        Instruction &I = *It++;
        if (!shouldInstrumentInstruction(I)) {
          continue;
        }

        if (AidInsertAfter) {
          if (I.isTerminator()) {
            continue;
          }
          if (auto *CB = dyn_cast<CallBase>(&I)) {
            if (CB->isMustTailCall()) {
              continue;
            }
          }
          Instruction *Next = I.getNextNode();
          if (!Next) {
            continue;
          }
          IRBuilder<> B(Next);
          if (Asm) {
            B.CreateCall(Asm);
          }
          if (AidPrint) {
            emitPrint(B, F, I, Puts);
          }
        } else {
          IRBuilder<> B(&I);
          if (Asm) {
            B.CreateCall(Asm);
          }
          if (AidPrint) {
            emitPrint(B, F, I, Puts);
          }
        }
      }
    }
  }

  return PreservedAnalyses::none();
}

}  // namespace auto_instrument

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION,
          "AutoInstrumentPass",
          "0.1",
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "auto-instrument") {
                    MPM.addPass(auto_instrument::AutoInstrumentPass());
                    return true;
                  }
                  if (Name == "intmon-branch") {
                    MPM.addPass(auto_instrument::IntMonBranchPass());
                    return true;
                  }
                  return false;
                });
          }};
}
