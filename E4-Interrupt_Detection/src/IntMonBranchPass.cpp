#include "AutoInstrument/IntMonBranchPass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

using namespace llvm;

namespace {

enum class InstrumentMode {
  None,
  All,
  Secret,
  List,
};

enum class SecretInstrumentMode {
  Once,
  Branch,
  Block,
};

enum class FuncInstrumentMode {
  Off,
  Instrumented,
  All,
};

static cl::opt<std::string> InstrumentModeOpt(
    "instrument-mode",
    cl::desc("插桩模式: all|secret|list"),
    cl::init("all"));

static cl::opt<std::string> InstrumentListPath(
    "instrument-list",
    cl::desc("仅在 instrument-mode=list 时生效，文件中每行一个 branch_id"),
    cl::init(""));

static cl::opt<std::string> EmitBranchMapPath(
    "emit-branch-map",
    cl::desc("输出分支映射文件：branch_id 与位置"),
    cl::init(""));

static cl::opt<std::string> DivergenceModeOpt(
    "divergence",
    cl::desc("控制流差异点插桩: on|off"),
    cl::init("off"));

static cl::opt<std::string> F2AtOpt(
    "f2-at",
    cl::desc("F2 插入位置: succ|join"),
    cl::init("succ"));

static cl::opt<std::string> SecretInstrumentOpt(
    "secret-instrument",
    cl::desc("secret 分支插桩范围: once|branch|block"),
    cl::init("once"));

static cl::list<std::string> SyscallFuncNamesOpt(
    "syscall-funcs",
    cl::desc("视为 syscall 边界的库函数名（逗号分隔，默认内置列表）"),
    cl::CommaSeparated);

static cl::list<std::string> SyscallFuncPrefixesOpt(
    "syscall-func-prefixes",
    cl::desc("视为 syscall 边界的库函数名前缀（逗号分隔）"),
    cl::CommaSeparated);

static cl::opt<std::string> FuncInstrumentOpt(
    "func-instrument",
    cl::desc("函数计时插桩范围: off|instrumented|all"),
    cl::init("instrumented"));

static cl::opt<bool> InstrumentFullOpt(
    "instrument-full",
    cl::desc("全函数逐指令插桩（跳过 PHI/调试/生命周期/终结指令）"),
    cl::init(false));

static cl::opt<bool> InlineGsOpt(
    "inline-gs",
    cl::desc("GS 模式下将 prepare/check 内联为汇编指令（仍保留 *_cnt 统计）"),
    cl::init(false));

static cl::opt<unsigned> DivergenceMaxNodes(
    "divergence-max-nodes",
    cl::desc("CFG 搜索最大节点数（防止循环）"),
    cl::init(5000));

static cl::list<std::string> SecretGlobals(
    "secret-globals",
    cl::desc("secret 全局变量名称（逗号分隔）"),
    cl::CommaSeparated);

static cl::list<std::string> SecretArgs(
    "secret-args",
    cl::desc("secret 形参列表：func:idx（逗号分隔）"),
    cl::CommaSeparated);

static cl::opt<std::string> SecretLinesPath(
    "secret-lines",
    cl::desc("通过源码行号标记 secret 分支点，文件每行：file:line 或 line"),
    cl::init(""));

static cl::opt<bool> SecretNameMatch(
    "secret-name",
    cl::desc("名称包含 secret 时视为 secret source"),
    cl::init(true));

struct BranchInfo {
  BranchInst *Inst;
  uint64_t Id;
  Function *Func;
  std::string FuncName;
  std::string BbName;
  unsigned BbIndex;
  unsigned InstIndex;
  bool IsSecret;
};

struct TaintState {
  DenseSet<const Value *> Values;
  DenseSet<const Value *> MemObjects;

  bool isValueTainted(const Value *V) const {
    return Values.contains(V);
  }

  bool isMemTainted(const Value *V) const {
    return MemObjects.contains(V);
  }
};

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

struct SecretLineEntry {
  std::string File;
  unsigned Line;
};

struct LineKey {
  std::string File;
  unsigned Line;

  bool operator==(const LineKey &Other) const {
    return Line == Other.Line && File == Other.File;
  }
};

struct LineKeyHash {
  std::size_t operator()(const LineKey &K) const {
    std::size_t H1 = std::hash<std::string>()(K.File);
    std::size_t H2 = std::hash<unsigned>()(K.Line);
    return H1 ^ (H2 + 0x9e3779b97f4a7c15ULL + (H1 << 6) + (H1 >> 2));
  }
};

uint64_t fnv1a64(StringRef S) {
  uint64_t Hash = kFnvOffset;
  for (unsigned char C : S) {
    Hash ^= C;
    Hash *= kFnvPrime;
  }
  return Hash;
}

uint64_t makeBranchId(const Function &F, const BasicBlock &BB, unsigned BbIndex,
                      unsigned InstIndex) {
  std::string Key;
  Key.reserve(F.getName().size() + 32);
  Key.append(F.getName().str());
  Key.push_back(':');
  if (BB.hasName()) {
    Key.append(BB.getName().str());
  } else {
    Key.append("bb");
    Key.append(std::to_string(BbIndex));
  }
  Key.push_back(':');
  Key.append(std::to_string(InstIndex));
  return fnv1a64(Key);
}

uint64_t makeFunctionId(const Function &F) {
  if (F.hasName()) {
    return fnv1a64(F.getName());
  }
  return fnv1a64("<anon>");
}

std::string toLower(StringRef S) {
  std::string Out;
  Out.reserve(S.size());
  for (char C : S) {
    Out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(C))));
  }
  return Out;
}

bool nameContainsSecret(StringRef Name) {
  if (!SecretNameMatch) {
    return false;
  }
  std::string Lower = toLower(Name);
  return Lower.find("secret") != std::string::npos;
}

InstrumentMode parseInstrumentMode(StringRef S) {
  std::string Lower = toLower(S);
  if (Lower == "none" || Lower == "off") {
    return InstrumentMode::None;
  }
  if (Lower == "all") {
    return InstrumentMode::All;
  }
  if (Lower == "secret") {
    return InstrumentMode::Secret;
  }
  if (Lower == "list") {
    return InstrumentMode::List;
  }
  errs() << "[intmon] 未知 instrument-mode: " << S
         << ", 使用 all\n";
  return InstrumentMode::All;
}

bool parseBoolMode(StringRef S, bool Default) {
  std::string Lower = toLower(S);
  if (Lower == "on") {
    return true;
  }
  if (Lower == "off") {
    return false;
  }
  return Default;
}

bool parseF2At(StringRef S, bool &UseJoin) {
  std::string Lower = toLower(S);
  if (Lower == "succ") {
    UseJoin = false;
    return true;
  }
  if (Lower == "join") {
    UseJoin = true;
    return true;
  }
  errs() << "[intmon] 未知 f2-at: " << S << ", 使用 succ\n";
  UseJoin = false;
  return false;
}

bool isInlineGsEnabled(const Module &M) {
  if (!InlineGsOpt) {
    return false;
  }
  const std::string Target = M.getTargetTriple();
  if (Target.empty()) {
    return false;
  }
  StringRef T(Target);
  return T.startswith("x86_64") || T.startswith("i386") || T.startswith("i486") ||
         T.startswith("i586") || T.startswith("i686");
}

void emitInlinePrepare(IRBuilder<> &B) {
  Type *I16 = B.getInt16Ty();
  Value *One = ConstantInt::get(I16, 1);
  auto *Asm = InlineAsm::get(FunctionType::get(B.getVoidTy(), {I16}, false),
                             "movw $0, %gs", "r,~{memory}", true);
  B.CreateCall(Asm, {One});
}

void emitInlineCheck(IRBuilder<> &B) {
  Type *I16 = B.getInt16Ty();
  Type *I64 = B.getInt64Ty();
  auto *Asm = InlineAsm::get(FunctionType::get(I16, {}, false),
                             "movw %gs, $0", "=r,~{memory}", true);
  Value *V = B.CreateCall(Asm, {});
  Value *IsZero = B.CreateICmpEQ(V, ConstantInt::get(I16, 0));
  Value *Inc = B.CreateZExt(IsZero, I64);
  Module *M = B.GetInsertBlock()->getModule();
  GlobalVariable *Counter =
      M->getGlobalVariable("intmon_interrupt_detect_count");
  if (!Counter) {
    Counter = cast<GlobalVariable>(
        M->getOrInsertGlobal("intmon_interrupt_detect_count", I64));
    Counter->setLinkage(GlobalValue::ExternalLinkage);
  }
  B.CreateAtomicRMW(AtomicRMWInst::Add, Counter, Inc, MaybeAlign(8),
                    AtomicOrdering::Monotonic);
}

SecretInstrumentMode parseSecretInstrument(StringRef S) {
  std::string Lower = toLower(S);
  if (Lower == "once") {
    return SecretInstrumentMode::Once;
  }
  if (Lower == "branch") {
    return SecretInstrumentMode::Branch;
  }
  if (Lower == "block") {
    return SecretInstrumentMode::Block;
  }
  errs() << "[intmon] 未知 secret-instrument: " << S << ", 使用 once\n";
  return SecretInstrumentMode::Once;
}

FuncInstrumentMode parseFuncInstrument(StringRef S) {
  std::string Lower = toLower(S);
  if (Lower == "off") {
    return FuncInstrumentMode::Off;
  }
  if (Lower == "all") {
    return FuncInstrumentMode::All;
  }
  if (Lower == "instrumented") {
    return FuncInstrumentMode::Instrumented;
  }
  errs() << "[intmon] 未知 func-instrument: " << S
         << ", 使用 instrumented\n";
  return FuncInstrumentMode::Instrumented;
}

StringSet<> buildSecretGlobalSet() {
  StringSet<> Set;
  for (const auto &Name : SecretGlobals) {
    Set.insert(Name);
  }
  return Set;
}

StringMap<SmallDenseSet<unsigned, 4>> buildSecretArgMap() {
  StringMap<SmallDenseSet<unsigned, 4>> Map;
  for (const auto &Entry : SecretArgs) {
    auto Pos = Entry.find(':');
    if (Pos == std::string::npos) {
      errs() << "[intmon] secret-args 格式无效: " << Entry << " (应为 func:idx)\n";
      continue;
    }
    std::string Func = Entry.substr(0, Pos);
    std::string IdxStr = Entry.substr(Pos + 1);
    unsigned Idx = 0;
    try {
      Idx = static_cast<unsigned>(std::stoul(IdxStr, nullptr, 0));
    } catch (...) {
      errs() << "[intmon] secret-args 索引无效: " << Entry << "\n";
      continue;
    }
    Map[Func].insert(Idx);
  }
  return Map;
}

bool isSecretArg(const Argument &Arg,
                 const StringMap<SmallDenseSet<unsigned, 4>> &SecretArgMap) {
  if (nameContainsSecret(Arg.getName())) {
    return true;
  }
  const Function *F = Arg.getParent();
  if (!F) {
    return false;
  }
  if (F->getAttributes().hasParamAttr(Arg.getArgNo(), "secret")) {
    return true;
  }
  auto It = SecretArgMap.find(F->getName());
  if (It == SecretArgMap.end()) {
    return false;
  }
  return It->second.contains(Arg.getArgNo());
}

bool isSecretGlobal(const GlobalVariable &GV, const StringSet<> &SecretGlobals) {
  if (GV.getMetadata("secret")) {
    return true;
  }
  if (SecretGlobals.contains(GV.getName())) {
    return true;
  }
  if (nameContainsSecret(GV.getName())) {
    return true;
  }
  return false;
}

SmallVector<SecretLineEntry, 16> readSecretLines(const std::string &Path) {
  SmallVector<SecretLineEntry, 16> Entries;
  if (Path.empty()) {
    return Entries;
  }
  std::ifstream In(Path);
  if (!In) {
    errs() << "[intmon] 无法读取 secret-lines: " << Path << "\n";
    return Entries;
  }
  std::string Raw;
  while (std::getline(In, Raw)) {
    StringRef LineRef(Raw);
    LineRef = LineRef.trim();
    if (LineRef.empty() || LineRef.starts_with("#")) {
      continue;
    }
    std::string FilePart;
    std::string LinePart;
    size_t Pos = LineRef.rfind(':');
    if (Pos != std::string::npos) {
      FilePart = LineRef.substr(0, Pos).str();
      LinePart = LineRef.substr(Pos + 1).str();
    } else {
      LinePart = LineRef.str();
    }
    unsigned LineNo = 0;
    try {
      LineNo = static_cast<unsigned>(std::stoul(LinePart, nullptr, 10));
    } catch (...) {
      errs() << "[intmon] secret-lines 无法解析: " << LineRef << "\n";
      continue;
    }
    SecretLineEntry Entry;
    Entry.File = FilePart;
    Entry.Line = LineNo;
    Entries.push_back(std::move(Entry));
  }
  return Entries;
}

bool matchSecretLine(const Instruction &I,
                     const SmallVectorImpl<SecretLineEntry> &Entries,
                     LineKey *Matched) {
  if (Entries.empty()) {
    return false;
  }
  DebugLoc Loc = I.getDebugLoc();
  if (!Loc) {
    return false;
  }
  unsigned Line = Loc.getLine();
  if (Line == 0) {
    return false;
  }
  StringRef File = Loc->getFilename();
  StringRef Dir = Loc->getDirectory();
  std::string FullPath;
  if (!Dir.empty()) {
    FullPath = (Dir + "/" + File).str();
  } else {
    FullPath = File.str();
  }
  StringRef FullRef(FullPath);
  StringRef BaseRef = File;
  for (const auto &Entry : Entries) {
    if (Entry.Line != Line) {
      continue;
    }
    if (Entry.File.empty()) {
      if (Matched) {
        Matched->File = Entry.File;
        Matched->Line = Entry.Line;
      }
      return true;
    }
    StringRef Target(Entry.File);
    if (BaseRef == Target) {
      if (Matched) {
        Matched->File = Entry.File;
        Matched->Line = Entry.Line;
      }
      return true;
    }
    if (FullRef.ends_with(Target)) {
      if (Matched) {
        Matched->File = Entry.File;
        Matched->Line = Entry.Line;
      }
      return true;
    }
  }
  return false;
}

const Value *getMemObject(const Value *Ptr, const DataLayout &DL) {
  if (!Ptr) {
    return nullptr;
  }
  (void)DL;
  return getUnderlyingObject(Ptr);
}

const Function *getCalleeFunction(const CallBase &CB) {
  const Value *Callee = CB.getCalledOperand();
  if (!Callee) {
    return nullptr;
  }
  Callee = Callee->stripPointerCasts();
  return dyn_cast<Function>(Callee);
}

bool isIntMonCallName(const CallBase &CB, StringRef Name) {
  const Function *Callee = getCalleeFunction(CB);
  if (!Callee) {
    return false;
  }
  return Callee->getName() == Name;
}

bool isIntMonCallWithId(const Instruction &I, StringRef Name, uint64_t Id,
                        std::optional<int32_t> Param) {
  const auto *CB = dyn_cast<CallBase>(&I);
  if (!CB) {
    return false;
  }
  if (!isIntMonCallName(*CB, Name)) {
    return false;
  }
  if (CB->arg_size() < 1) {
    return false;
  }
  const auto *IdConst = dyn_cast<ConstantInt>(CB->getArgOperand(0));
  if (!IdConst) {
    return false;
  }
  if (IdConst->getZExtValue() != Id) {
    return false;
  }
  if (!Param.has_value()) {
    return true;
  }
  if (CB->arg_size() < 2) {
    return false;
  }
  const auto *ParamConst = dyn_cast<ConstantInt>(CB->getArgOperand(1));
  if (!ParamConst) {
    return false;
  }
  return ParamConst->getSExtValue() == *Param;
}

bool isIntMonCall(const Instruction &I) {
  const auto *CB = dyn_cast<CallBase>(&I);
  if (!CB) {
    return false;
  }
  return isIntMonCallName(*CB, "__intmon_prepare") ||
         isIntMonCallName(*CB, "__intmon_check") ||
         isIntMonCallName(*CB, "__intmon_prepare_cnt") ||
         isIntMonCallName(*CB, "__intmon_check_cnt") ||
         isIntMonCallName(*CB, "__intmon_div") ||
         isIntMonCallName(*CB, "__intmon_func_enter") ||
         isIntMonCallName(*CB, "__intmon_func_exit");
}

bool shouldCountForBranchId(const Instruction &I) {
  if (isa<DbgInfoIntrinsic>(I) || isa<LifetimeIntrinsic>(I)) {
    return false;
  }
  if (isIntMonCall(I)) {
    return false;
  }
  return true;
}

bool shouldInstrumentInstruction(const Instruction &I) {
  if (isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I) ||
      isa<LifetimeIntrinsic>(I)) {
    return false;
  }
  if (isIntMonCall(I)) {
    return false;
  }
  if (I.isTerminator()) {
    return false;
  }
  return true;
}

StringRef stripSyscallDecorations(StringRef Name) {
  if (Name.endswith(".plt")) {
    Name = Name.drop_back(4);
  }
  size_t At = Name.find('@');
  if (At != StringRef::npos) {
    Name = Name.substr(0, At);
  }
  return Name;
}

bool isSyscallLikeName(StringRef Name) {
  std::string Lower = toLower(Name);
  StringRef N(Lower);
  N = stripSyscallDecorations(N);
  if (N == "syscall" || N.startswith("__syscall")) {
    return true;
  }

  static const char *DefaultSyscallFuncs[] = {
      "read",     "write",     "open",      "openat",   "close",   "ioctl",
      "fcntl",    "lseek",     "pread",     "pwrite",   "readv",   "writev",
      "mmap",     "munmap",    "mprotect",  "brk",      "futex",   "nanosleep",
      "clock_gettime", "gettimeofday", "sched_yield",
      "poll",     "ppoll",     "select",    "pselect",  "epoll_wait",
      "socket",   "connect",   "accept",    "bind",     "listen",  "shutdown",
      "send",     "sendto",    "recv",      "recvfrom",
      "fork",     "vfork",     "clone",     "execve",   "wait4",   "waitpid",
      "kill",     "tgkill",    "tkill",     "prctl",    "uname",
      "exit",     "exit_group",
      "printf",   "fprintf",   "sprintf",   "snprintf", "puts",    "fputs",
      "putchar",  "perror",    "fflush",    "fwrite",   "fread",
      "fgetc",    "fgets",     "fputc",     "fseek",    "ftell",
      "malloc",   "calloc",    "realloc",   "free",
  };

  if (!SyscallFuncNamesOpt.empty()) {
    for (const std::string &S : SyscallFuncNamesOpt) {
      if (N == toLower(S)) {
        return true;
      }
    }
  } else {
    for (const char *S : DefaultSyscallFuncs) {
      if (N == S) {
        return true;
      }
    }
  }

  if (!SyscallFuncPrefixesOpt.empty()) {
    for (const std::string &S : SyscallFuncPrefixesOpt) {
      if (!S.empty() && N.startswith(toLower(S))) {
        return true;
      }
    }
  }
  return false;
}

bool isSyscallInstruction(const Instruction &I) {
  auto *CB = dyn_cast<CallBase>(&I);
  if (!CB) {
    return false;
  }
  if (const Function *Callee = CB->getCalledFunction()) {
    StringRef Name = Callee->getName();
    if (isSyscallLikeName(Name)) {
      return true;
    }
  }
  const Value *CalleeV = CB->getCalledOperand();
  if (CalleeV) {
    CalleeV = CalleeV->stripPointerCasts();
  }
  if (const auto *IA = dyn_cast_or_null<InlineAsm>(CalleeV)) {
    std::string Asm = toLower(IA->getAsmString());
    if (Asm.find("syscall") != std::string::npos ||
        Asm.find("sysenter") != std::string::npos ||
        Asm.find("int $0x80") != std::string::npos ||
        Asm.find("int $128") != std::string::npos ||
        Asm.find("int 0x80") != std::string::npos ||
        Asm.find("int 128") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool hasExistingPrepareBeforeInst(const Instruction &I, uint64_t Id) {
  const Instruction *Cur = I.getPrevNode();
  int Scanned = 0;
  while (Cur && Scanned < 6) {
    if (isa<DbgInfoIntrinsic>(*Cur) || isa<LifetimeIntrinsic>(*Cur)) {
      Cur = Cur->getPrevNode();
      continue;
    }
    if (isIntMonCallWithId(*Cur, "__intmon_prepare", Id, std::nullopt)) {
      return true;
    }
    if (!isIntMonCall(*Cur)) {
      break;
    }
    Cur = Cur->getPrevNode();
    Scanned++;
  }
  return false;
}

bool hasExistingPrepareCntBeforeInst(const Instruction &I, uint64_t Id) {
  const Instruction *Cur = I.getPrevNode();
  int Scanned = 0;
  while (Cur && Scanned < 6) {
    if (isa<DbgInfoIntrinsic>(*Cur) || isa<LifetimeIntrinsic>(*Cur)) {
      Cur = Cur->getPrevNode();
      continue;
    }
    if (isIntMonCallWithId(*Cur, "__intmon_prepare_cnt", Id, std::nullopt)) {
      return true;
    }
    if (!isIntMonCall(*Cur)) {
      break;
    }
    Cur = Cur->getPrevNode();
    Scanned++;
  }
  return false;
}

bool hasExistingCheckBeforeInst(const Instruction &I, uint64_t Id,
                                int32_t Path) {
  const Instruction *Cur = I.getPrevNode();
  int Scanned = 0;
  while (Cur && Scanned < 6) {
    if (isa<DbgInfoIntrinsic>(*Cur) || isa<LifetimeIntrinsic>(*Cur)) {
      Cur = Cur->getPrevNode();
      continue;
    }
    if (isIntMonCallWithId(*Cur, "__intmon_check", Id, Path)) {
      return true;
    }
    if (!isIntMonCall(*Cur)) {
      break;
    }
    Cur = Cur->getPrevNode();
    Scanned++;
  }
  return false;
}

bool hasExistingCheckCntBeforeInst(const Instruction &I, uint64_t Id,
                                   int32_t Path) {
  const Instruction *Cur = I.getPrevNode();
  int Scanned = 0;
  while (Cur && Scanned < 6) {
    if (isa<DbgInfoIntrinsic>(*Cur) || isa<LifetimeIntrinsic>(*Cur)) {
      Cur = Cur->getPrevNode();
      continue;
    }
    if (isIntMonCallWithId(*Cur, "__intmon_check_cnt", Id, Path)) {
      return true;
    }
    if (!isIntMonCall(*Cur)) {
      break;
    }
    Cur = Cur->getPrevNode();
    Scanned++;
  }
  return false;
}

bool hasExistingCheckAfterInst(const Instruction &I, uint64_t Id,
                               int32_t Path) {
  const Instruction *Cur = I.getNextNode();
  int Scanned = 0;
  while (Cur && Scanned < 6) {
    if (isa<DbgInfoIntrinsic>(*Cur) || isa<LifetimeIntrinsic>(*Cur)) {
      Cur = Cur->getNextNode();
      continue;
    }
    if (isIntMonCallWithId(*Cur, "__intmon_check", Id, Path)) {
      return true;
    }
    if (!isIntMonCall(*Cur)) {
      break;
    }
    Cur = Cur->getNextNode();
    Scanned++;
  }
  return false;
}

bool hasExistingCheckCntAfterInst(const Instruction &I, uint64_t Id,
                                  int32_t Path) {
  const Instruction *Cur = I.getNextNode();
  int Scanned = 0;
  while (Cur && Scanned < 6) {
    if (isa<DbgInfoIntrinsic>(*Cur) || isa<LifetimeIntrinsic>(*Cur)) {
      Cur = Cur->getNextNode();
      continue;
    }
    if (isIntMonCallWithId(*Cur, "__intmon_check_cnt", Id, Path)) {
      return true;
    }
    if (!isIntMonCall(*Cur)) {
      break;
    }
    Cur = Cur->getNextNode();
    Scanned++;
  }
  return false;
}

bool hasExistingF1Before(const Instruction &Br, uint64_t Id) {
  const Instruction *Cur = Br.getPrevNode();
  int Scanned = 0;
  while (Cur && Scanned < 4) {
    if (isa<DbgInfoIntrinsic>(*Cur) || isa<LifetimeIntrinsic>(*Cur)) {
      Cur = Cur->getPrevNode();
      continue;
    }
    if (isIntMonCallWithId(*Cur, "__intmon_prepare", Id, std::nullopt)) {
      return true;
    }
    Cur = Cur->getPrevNode();
    Scanned++;
  }
  return false;
}

bool hasExistingF1CountBefore(const Instruction &Br, uint64_t Id) {
  const Instruction *Cur = Br.getPrevNode();
  int Scanned = 0;
  while (Cur && Scanned < 4) {
    if (isa<DbgInfoIntrinsic>(*Cur) || isa<LifetimeIntrinsic>(*Cur)) {
      Cur = Cur->getPrevNode();
      continue;
    }
    if (isIntMonCallWithId(*Cur, "__intmon_prepare_cnt", Id, std::nullopt)) {
      return true;
    }
    Cur = Cur->getPrevNode();
    Scanned++;
  }
  return false;
}

bool hasExistingF2AtEntry(BasicBlock &BB, uint64_t Id, int32_t Path) {
  int Scanned = 0;
  for (Instruction &I : BB) {
    if (isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I) ||
        isa<LifetimeIntrinsic>(I)) {
      continue;
    }
    if (isIntMonCallWithId(I, "__intmon_check", Id, Path)) {
      return true;
    }
    Scanned++;
    if (Scanned >= 4) {
      break;
    }
  }
  return false;
}

bool hasExistingF2CountAtEntry(BasicBlock &BB, uint64_t Id, int32_t Path) {
  int Scanned = 0;
  for (Instruction &I : BB) {
    if (isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I) ||
        isa<LifetimeIntrinsic>(I)) {
      continue;
    }
    if (isIntMonCallWithId(I, "__intmon_check_cnt", Id, Path)) {
      return true;
    }
    Scanned++;
    if (Scanned >= 4) {
      break;
    }
  }
  return false;
}

bool hasExistingF2AtEntryAnyPath(BasicBlock &BB, uint64_t Id) {
  int Scanned = 0;
  for (Instruction &I : BB) {
    if (isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I) ||
        isa<LifetimeIntrinsic>(I)) {
      continue;
    }
    if (isIntMonCallWithId(I, "__intmon_check", Id, std::nullopt)) {
      return true;
    }
    Scanned++;
    if (Scanned >= 4) {
      break;
    }
  }
  return false;
}

bool hasExistingF2CountAtEntryAnyPath(BasicBlock &BB, uint64_t Id) {
  int Scanned = 0;
  for (Instruction &I : BB) {
    if (isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I) ||
        isa<LifetimeIntrinsic>(I)) {
      continue;
    }
    if (isIntMonCallWithId(I, "__intmon_check_cnt", Id, std::nullopt)) {
      return true;
    }
    Scanned++;
    if (Scanned >= 4) {
      break;
    }
  }
  return false;
}

bool hasExistingDivAtEntry(BasicBlock &BB, uint64_t Id, int32_t Kind) {
  int Scanned = 0;
  for (Instruction &I : BB) {
    if (isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I) ||
        isa<LifetimeIntrinsic>(I)) {
      continue;
    }
    if (isIntMonCallWithId(I, "__intmon_div", Id, Kind)) {
      return true;
    }
    Scanned++;
    if (Scanned >= 4) {
      break;
    }
  }
  return false;
}

bool hasExistingFuncEnterAtEntry(BasicBlock &BB, uint64_t Id) {
  int Scanned = 0;
  for (Instruction &I : BB) {
    if (isa<PHINode>(I) || isa<DbgInfoIntrinsic>(I) ||
        isa<LifetimeIntrinsic>(I)) {
      continue;
    }
    if (isIntMonCallWithId(I, "__intmon_func_enter", Id, std::nullopt)) {
      return true;
    }
    Scanned++;
    if (Scanned >= 4) {
      break;
    }
  }
  return false;
}

bool hasExistingFuncExitBefore(const Instruction &Term, uint64_t Id) {
  const Instruction *Cur = Term.getPrevNode();
  int Scanned = 0;
  while (Cur && Scanned < 6) {
    if (isa<DbgInfoIntrinsic>(*Cur) || isa<LifetimeIntrinsic>(*Cur)) {
      Cur = Cur->getPrevNode();
      continue;
    }
    if (isIntMonCallWithId(*Cur, "__intmon_func_exit", Id, std::nullopt)) {
      return true;
    }
    Cur = Cur->getPrevNode();
    Scanned++;
  }
  return false;
}

bool isEhBlock(const BasicBlock &BB) {
  if (BB.isEHPad()) {
    return true;
  }
  for (const Instruction &I : BB) {
    if (isa<LandingPadInst>(I)) {
      return true;
    }
  }
  if (const Instruction *Term = BB.getTerminator()) {
    if (isa<InvokeInst>(Term)) {
      return true;
    }
  }
  return false;
}

bool collectReachableBlocks(BasicBlock *Start, BasicBlock *Stop,
                            BasicBlock *DomBB, DominatorTree &DT,
                            DenseSet<BasicBlock *> &Out,
                            unsigned MaxNodes) {
  if (!Start || Start == Stop) {
    return true;
  }
  if (isEhBlock(*Start)) {
    return true;
  }
  if (DomBB && !DT.dominates(DomBB, Start)) {
    return true;
  }

  SmallVector<BasicBlock *, 64> Queue;
  DenseSet<BasicBlock *> Visited;
  Queue.push_back(Start);
  Visited.insert(Start);

  size_t Head = 0;
  while (Head < Queue.size()) {
    if (Visited.size() > MaxNodes) {
      return false;
    }
    BasicBlock *Cur = Queue[Head++];
    if (!Cur || Cur == Stop) {
      continue;
    }
    Out.insert(Cur);
    for (BasicBlock *Succ : successors(Cur)) {
      if (!Succ || Succ == Stop) {
        continue;
      }
      if (isEhBlock(*Succ)) {
        continue;
      }
      if (DomBB && !DT.dominates(DomBB, Succ)) {
        continue;
      }
      if (Visited.insert(Succ).second) {
        Queue.push_back(Succ);
      }
    }
  }
  return true;
}

bool bfsFindPath(BasicBlock *Start, BasicBlock *Target,
                 SmallVectorImpl<BasicBlock *> &Path,
                 unsigned MaxNodes) {
  Path.clear();
  if (!Start || !Target) {
    return false;
  }
  if (Start == Target) {
    Path.push_back(Start);
    return true;
  }

  DenseMap<BasicBlock *, BasicBlock *> Prev;
  SmallVector<BasicBlock *, 64> Queue;
  DenseSet<BasicBlock *> Visited;

  Queue.push_back(Start);
  Visited.insert(Start);

  size_t Head = 0;
  while (Head < Queue.size()) {
    if (Visited.size() > MaxNodes) {
      return false;
    }
    BasicBlock *Cur = Queue[Head++];
    for (BasicBlock *Succ : successors(Cur)) {
      if (!Succ || isEhBlock(*Succ)) {
        continue;
      }
      if (Visited.contains(Succ)) {
        continue;
      }
      Visited.insert(Succ);
      Prev[Succ] = Cur;
      if (Succ == Target) {
        BasicBlock *Walk = Target;
        while (Walk) {
          Path.push_back(Walk);
          auto It = Prev.find(Walk);
          if (It == Prev.end()) {
            break;
          }
          Walk = It->second;
        }
        std::reverse(Path.begin(), Path.end());
        return true;
      }
      Queue.push_back(Succ);
    }
  }
  return false;
}

bool classifyJoinPreds(BranchInst *BI, BasicBlock *TrueBB, BasicBlock *FalseBB,
                       BasicBlock *JoinBB, DominatorTree &DT,
                       SmallVectorImpl<BasicBlock *> &TruePreds,
                       SmallVectorImpl<BasicBlock *> &FalsePreds) {
  if (!BI || !TrueBB || !FalseBB || !JoinBB) {
    return false;
  }
  BasicBlock *ParentBB = BI->getParent();
  bool UnknownPred = false;
  for (BasicBlock *Pred : predecessors(JoinBB)) {
    if (Pred == ParentBB && JoinBB == TrueBB) {
      TruePreds.push_back(Pred);
      continue;
    }
    if (Pred == ParentBB && JoinBB == FalseBB) {
      FalsePreds.push_back(Pred);
      continue;
    }
    if (DT.dominates(TrueBB, Pred)) {
      TruePreds.push_back(Pred);
    } else if (DT.dominates(FalseBB, Pred)) {
      FalsePreds.push_back(Pred);
    } else {
      UnknownPred = true;
      break;
    }
  }
  if (UnknownPred || TruePreds.empty() || FalsePreds.empty()) {
    return false;
  }
  return true;
}

bool readInstrumentList(const std::string &Path, DenseSet<uint64_t> &Out) {
  if (Path.empty()) {
    return true;
  }
  ErrorOr<std::unique_ptr<MemoryBuffer>> Buffer =
      MemoryBuffer::getFile(Path);
  if (!Buffer) {
    errs() << "[intmon] 无法读取 instrument-list: " << Path << "\n";
    return false;
  }
  StringRef Data = Buffer->get()->getBuffer();
  SmallVector<StringRef, 64> Lines;
  Data.split(Lines, '\n');
  for (StringRef Line : Lines) {
    Line = Line.trim();
    if (Line.empty() || Line.starts_with("#")) {
      continue;
    }
    uint64_t Id = 0;
    if (Line.starts_with("0x") || Line.starts_with("0X")) {
      if (Line.drop_front(2).getAsInteger(16, Id)) {
        errs() << "[intmon] 无法解析 branch_id: " << Line << "\n";
        continue;
      }
    } else {
      if (Line.getAsInteger(10, Id)) {
        errs() << "[intmon] 无法解析 branch_id: " << Line << "\n";
        continue;
      }
    }
    Out.insert(Id);
  }
  return true;
}

bool writeBranchMap(const std::string &Path,
                    const SmallVectorImpl<BranchInfo> &Branches) {
  if (Path.empty()) {
    return true;
  }
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "[intmon] 无法写入 branch map: " << Path << "\n";
    return false;
  }
  for (const auto &B : Branches) {
    OS << B.Id << "\t" << B.FuncName << "\t" << B.BbName << "\t"
       << B.InstIndex;
    DebugLoc Loc = B.Inst->getDebugLoc();
    if (Loc) {
      StringRef File = Loc->getFilename();
      StringRef Dir = Loc->getDirectory();
      if (!Dir.empty()) {
        OS << "\t" << Dir << "/" << File;
      } else {
        OS << "\t" << File;
      }
      OS << "\t" << Loc.getLine() << ":" << Loc.getCol();
    } else {
      OS << "\t" << "-" << "\t" << "-";
    }
    OS << "\n";
  }
  return true;
}

TaintState computeTaint(Function &F, const DataLayout &DL,
                        const StringSet<> &SecretGlobalSet,
                        const StringMap<SmallDenseSet<unsigned, 4>> &SecretArgMap) {
  TaintState State;

  unsigned InstrCount = 0;
  for (BasicBlock &BB : F) {
    InstrCount += static_cast<unsigned>(BB.size());
  }

  for (Argument &Arg : F.args()) {
    if (isSecretArg(Arg, SecretArgMap)) {
      State.Values.insert(&Arg);
    }
  }

  Module *M = F.getParent();
  if (M) {
    for (GlobalVariable &GV : M->globals()) {
      if (isSecretGlobal(GV, SecretGlobalSet)) {
        State.Values.insert(&GV);
        State.MemObjects.insert(&GV);
      }
    }
  }

  bool Changed = true;
  unsigned MaxIters = std::max(1u, InstrCount + 1);
  unsigned Iter = 0;
  while (Changed && Iter++ < MaxIters) {
    Changed = false;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (I.getMetadata("secret")) {
          if (!I.getType()->isVoidTy() && !State.Values.contains(&I)) {
            State.Values.insert(&I);
            Changed = true;
          }
        }

        if (auto *LI = dyn_cast<LoadInst>(&I)) {
          bool Tainted = State.Values.contains(LI->getPointerOperand());
          const Value *Obj =
              getMemObject(LI->getPointerOperand(), DL);
          if (Obj && State.MemObjects.contains(Obj)) {
            Tainted = true;
          }
          if (Tainted && !State.Values.contains(LI)) {
            State.Values.insert(LI);
            Changed = true;
          }
          continue;
        }

        if (auto *SI = dyn_cast<StoreInst>(&I)) {
          bool Tainted = State.Values.contains(SI->getValueOperand()) ||
                         State.Values.contains(SI->getPointerOperand());
          if (Tainted) {
            const Value *Obj =
                getMemObject(SI->getPointerOperand(), DL);
            if (Obj && State.MemObjects.insert(Obj).second) {
              Changed = true;
            }
          }
          continue;
        }

        if (auto *ARMW = dyn_cast<AtomicRMWInst>(&I)) {
          bool Tainted = State.Values.contains(ARMW->getValOperand()) ||
                         State.Values.contains(ARMW->getPointerOperand());
          if (Tainted) {
            const Value *Obj =
                getMemObject(ARMW->getPointerOperand(), DL);
            if (Obj && State.MemObjects.insert(Obj).second) {
              Changed = true;
            }
          }
          if (!State.Values.contains(ARMW) && Tainted) {
            State.Values.insert(ARMW);
            Changed = true;
          }
          continue;
        }

        if (auto *ACX = dyn_cast<AtomicCmpXchgInst>(&I)) {
          bool Tainted = State.Values.contains(ACX->getCompareOperand()) ||
                         State.Values.contains(ACX->getNewValOperand()) ||
                         State.Values.contains(ACX->getPointerOperand());
          if (Tainted) {
            const Value *Obj =
                getMemObject(ACX->getPointerOperand(), DL);
            if (Obj && State.MemObjects.insert(Obj).second) {
              Changed = true;
            }
          }
          if (!State.Values.contains(ACX) && Tainted) {
            State.Values.insert(ACX);
            Changed = true;
          }
          continue;
        }

        if (auto *CB = dyn_cast<CallBase>(&I)) {
          bool AnyArgTainted = false;
          for (unsigned i = 0; i < CB->arg_size(); ++i) {
            Value *Arg = CB->getArgOperand(i);
            if (State.Values.contains(Arg)) {
              AnyArgTainted = true;
              break;
            }
          }

          if (AnyArgTainted && !CB->getType()->isVoidTy() &&
              !State.Values.contains(CB)) {
            State.Values.insert(CB);
            Changed = true;
          }

          if (AnyArgTainted) {
            for (unsigned i = 0; i < CB->arg_size(); ++i) {
              Value *Arg = CB->getArgOperand(i);
              if (!Arg->getType()->isPointerTy()) {
                continue;
              }
              const Value *Obj = getMemObject(Arg, DL);
              if (Obj && State.MemObjects.insert(Obj).second) {
                Changed = true;
              }
            }
          }
          continue;
        }

        if (!I.getType()->isVoidTy()) {
          bool AnyOperandTainted = false;
          for (Value *Op : I.operands()) {
            if (State.Values.contains(Op)) {
              AnyOperandTainted = true;
              break;
            }
          }
          if (AnyOperandTainted && !State.Values.contains(&I)) {
            State.Values.insert(&I);
            Changed = true;
          }
        }
      }
    }
  }

  return State;
}

}  // namespace

namespace auto_instrument {

PreservedAnalyses IntMonBranchPass::run(Module &M, ModuleAnalysisManager &MAM) {
  LLVMContext &Ctx = M.getContext();

  FunctionCallee F1 = M.getOrInsertFunction(
      "__intmon_prepare", FunctionType::get(Type::getVoidTy(Ctx),
                                       {Type::getInt64Ty(Ctx)}, false));
  FunctionCallee F2 = M.getOrInsertFunction(
      "__intmon_check",
      FunctionType::get(Type::getVoidTy(Ctx),
                        {Type::getInt64Ty(Ctx), Type::getInt32Ty(Ctx)}, false));
  FunctionCallee F1Cnt = M.getOrInsertFunction(
      "__intmon_prepare_cnt", FunctionType::get(Type::getVoidTy(Ctx),
                                           {Type::getInt64Ty(Ctx)}, false));
  FunctionCallee F2Cnt = M.getOrInsertFunction(
      "__intmon_check_cnt",
      FunctionType::get(Type::getVoidTy(Ctx),
                        {Type::getInt64Ty(Ctx), Type::getInt32Ty(Ctx)}, false));
  FunctionCallee FDiv = M.getOrInsertFunction(
      "__intmon_div",
      FunctionType::get(Type::getVoidTy(Ctx),
                        {Type::getInt64Ty(Ctx), Type::getInt32Ty(Ctx)}, false));
  FunctionCallee FEnter = M.getOrInsertFunction(
      "__intmon_func_enter", FunctionType::get(Type::getVoidTy(Ctx),
                                               {Type::getInt64Ty(Ctx)}, false));
  FunctionCallee FExit = M.getOrInsertFunction(
      "__intmon_func_exit", FunctionType::get(Type::getVoidTy(Ctx),
                                              {Type::getInt64Ty(Ctx)}, false));

  InstrumentMode Mode = parseInstrumentMode(InstrumentModeOpt);
  bool DivergenceOn = parseBoolMode(DivergenceModeOpt, false);
  bool F2AtJoin = false;
  parseF2At(F2AtOpt, F2AtJoin);
  SecretInstrumentMode SecretInstr = parseSecretInstrument(SecretInstrumentOpt);
  bool SecretBranch = (SecretInstr == SecretInstrumentMode::Branch);
  bool SecretBlock = (SecretInstr == SecretInstrumentMode::Block);
  FuncInstrumentMode FuncInstr = parseFuncInstrument(FuncInstrumentOpt);
  bool InstrumentFull = InstrumentFullOpt;
  bool InlineGs = isInlineGsEnabled(M);
  if (InlineGsOpt && !InlineGs) {
    errs() << "[intmon] inline-gs 仅支持 x86 目标，已忽略\n";
  }
  auto EmitPrepare = [&](IRBuilder<> &B, Value *IdV) {
    if (InlineGs) {
      emitInlinePrepare(B);
    } else {
      B.CreateCall(F1, {IdV});
    }
  };
  auto EmitCheck = [&](IRBuilder<> &B, Value *IdV, Value *PathV) {
    if (InlineGs) {
      emitInlineCheck(B);
    } else {
      B.CreateCall(F2, {IdV, PathV});
    }
  };

  DenseSet<uint64_t> ListIds;
  if (Mode == InstrumentMode::List) {
    if (!readInstrumentList(InstrumentListPath, ListIds)) {
      return PreservedAnalyses::all();
    }
  }

  StringSet<> SecretGlobalSet = buildSecretGlobalSet();
  StringMap<SmallDenseSet<unsigned, 4>> SecretArgMap = buildSecretArgMap();
  SmallVector<SecretLineEntry, 16> SecretLines = readSecretLines(SecretLinesPath);
  std::unordered_map<LineKey, unsigned, LineKeyHash> SecretLineHits;

  SmallVector<BranchInfo, 64> Branches;
  unsigned TotalBranches = 0;
  unsigned SecretBranches = 0;

  for (Function &F : M) {
    if (F.isDeclaration()) {
      continue;
    }

    TaintState Taint =
        computeTaint(F, M.getDataLayout(), SecretGlobalSet, SecretArgMap);

    unsigned BbIndex = 0;
    for (BasicBlock &BB : F) {
      unsigned InstIndex = 0;
      for (Instruction &I : BB) {
        if (auto *BI = dyn_cast<BranchInst>(&I)) {
          if (BI->isConditional()) {
            uint64_t Id = makeBranchId(F, BB, BbIndex, InstIndex);
            LineKey Matched;
            bool FromLines = matchSecretLine(*BI, SecretLines, &Matched);
            if (FromLines) {
              SecretLineHits[Matched] += 1;
            }
            bool IsSecret = Taint.isValueTainted(BI->getCondition()) || FromLines;
            if (IsSecret) {
              SecretBranches++;
            }
            TotalBranches++;
            Branches.push_back({BI,
                                Id,
                                &F,
                                F.getName().str(),
                                BB.hasName() ? BB.getName().str()
                                             : ("bb" + std::to_string(BbIndex)),
                                BbIndex,
                                InstIndex,
                                IsSecret});
          }
        }
        if (shouldCountForBranchId(I)) {
          InstIndex++;
        }
      }
      BbIndex++;
    }
  }

  if (!EmitBranchMapPath.empty()) {
    writeBranchMap(EmitBranchMapPath, Branches);
  }

  errs() << "[intmon] conditional branches: " << TotalBranches
         << ", secret-dependent: " << SecretBranches << "\n";
  for (const auto &B : Branches) {
    if (B.IsSecret) {
      errs() << "[intmon] secret-branch id=" << B.Id << " func="
             << B.FuncName << " bb=" << B.BbName << " idx=" << B.InstIndex
             << "\n";
    }
  }
  if (!SecretLineHits.empty()) {
    errs() << "[intmon] secret-lines hit summary:\n";
    for (const auto &It : SecretLineHits) {
      const LineKey &K = It.first;
      if (!K.File.empty()) {
        errs() << "  " << K.File << ":" << K.Line << " -> " << It.second
               << "\n";
      } else {
        errs() << "  line:" << K.Line << " -> " << It.second << "\n";
      }
    }
    if (!EmitBranchMapPath.empty()) {
      std::string SummaryPath = EmitBranchMapPath + ".summary";
      std::error_code EC;
      raw_fd_ostream OS(SummaryPath, EC, sys::fs::OF_Text);
      if (EC) {
        errs() << "[intmon] 无法写入 secret-lines summary: " << SummaryPath
               << "\n";
      } else {
        for (const auto &It : SecretLineHits) {
          const LineKey &K = It.first;
          if (!K.File.empty()) {
            OS << K.File << ":" << K.Line << "\t" << It.second << "\n";
          } else {
            OS << "line:" << K.Line << "\t" << It.second << "\n";
          }
        }
      }
    }
  }

  bool Changed = false;
  auto &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  auto logDivPoint = [](uint64_t Id, int32_t Kind, const BasicBlock &BB,
                        const Function &F) {
    errs() << "[intmon] divergence id=" << Id << " kind=" << Kind
           << " func=" << F.getName() << " bb=";
    if (BB.hasName()) {
      errs() << BB.getName();
    } else {
      errs() << "<anon>";
    }
    errs() << "\n";
  };

  DenseSet<Function *> InstrumentedFuncs;

  if (InstrumentFull) {
    // 先收集所有待插桩指令，避免在遍历 BB 时插入新指令导致迭代器
    // 访问到新插入的指令（尤其 inline-gs 模式下 emitInlineCheck 产生的
    // ICmp/ZExt/AtomicRMW 不被 isIntMonCall 识别，会被再次插桩，
    // 引发级联膨胀直至 OOM）。
    struct FullInstInfo {
      Instruction *I;
      uint64_t Id;
    };
    SmallVector<FullInstInfo, 256> FullInsts;

    for (Function &F : M) {
      if (F.isDeclaration()) {
        continue;
      }
      InstrumentedFuncs.insert(&F);
      unsigned BbIndex = 0;
      for (BasicBlock &BB : F) {
        unsigned InstIndex = 0;
        for (Instruction &I : BB) {
          if (!shouldInstrumentInstruction(I)) {
            if (shouldCountForBranchId(I)) {
              InstIndex++;
            }
            continue;
          }
          uint64_t Id = makeBranchId(F, BB, BbIndex, InstIndex);
          FullInsts.push_back({&I, Id});
          if (shouldCountForBranchId(I)) {
            InstIndex++;
          }
        }
        BbIndex++;
      }
    }

    for (const auto &EI : FullInsts) {
      Instruction &I = *EI.I;
      uint64_t Id = EI.Id;
      Value *IdV = ConstantInt::get(Type::getInt64Ty(Ctx), Id);
      Value *PathV = ConstantInt::get(Type::getInt32Ty(Ctx), 2);

      if (!hasExistingPrepareBeforeInst(I, Id)) {
        IRBuilder<> B(&I);
        EmitPrepare(B, IdV);
        Changed = true;
      }
      if (!hasExistingPrepareCntBeforeInst(I, Id)) {
        IRBuilder<> B(&I);
        B.CreateCall(F1Cnt, {IdV});
        Changed = true;
      }
      if (Instruction *Next = I.getNextNode()) {
        if (!hasExistingCheckAfterInst(I, Id, 2)) {
          IRBuilder<> B(Next);
          EmitCheck(B, IdV, PathV);
          Changed = true;
        }
        if (!hasExistingCheckCntAfterInst(I, Id, 2)) {
          IRBuilder<> B(Next);
          B.CreateCall(F2Cnt, {IdV, PathV});
          Changed = true;
        }
      }
    }
  }

  if (!InstrumentFull) {
  for (const BranchInfo &Info : Branches) {
    BranchInst *BI = Info.Inst;
    if (!BI || !BI->isConditional()) {
      continue;
    }

    bool ShouldInstrument = false;
    switch (Mode) {
      case InstrumentMode::None:
        ShouldInstrument = false;
        break;
      case InstrumentMode::All:
        ShouldInstrument = true;
        break;
      case InstrumentMode::Secret:
        ShouldInstrument = Info.IsSecret;
        break;
      case InstrumentMode::List:
        ShouldInstrument = ListIds.contains(Info.Id);
        break;
    }

    // instrument-mode=none 时不插入 prepare/check，但仍需记录 secret
    // 函数，以便 func-instrument=instrumented 能正确插入 func_enter/exit。
    if (Info.IsSecret) {
      InstrumentedFuncs.insert(Info.Func);
    }

    bool ShouldDiverge = DivergenceOn && ShouldInstrument;

    if (!ShouldInstrument && !ShouldDiverge) {
      continue;
    }

    uint64_t Id = Info.Id;
    Value *IdV = ConstantInt::get(Type::getInt64Ty(Ctx), Id);

    if (ShouldInstrument) {
      InstrumentedFuncs.insert(Info.Func);
      if (!hasExistingF1Before(*BI, Id)) {
        IRBuilder<> B(BI);
        EmitPrepare(B, IdV);
        Changed = true;
      }
      if (!hasExistingF1CountBefore(*BI, Id)) {
        IRBuilder<> B(BI);
        B.CreateCall(F1Cnt, {IdV});
        Changed = true;
      }

      BasicBlock *TrueBB = BI->getSuccessor(0);
      BasicBlock *FalseBB = BI->getSuccessor(1);

      bool BranchSecretInstr = SecretBranch && Info.IsSecret;
      bool BlockSecretInstr = SecretBlock && Info.IsSecret;
      bool UseJoinForBranch =
          (BlockSecretInstr || (F2AtJoin && !BranchSecretInstr));

      if (!UseJoinForBranch) {
        if (TrueBB && !hasExistingF2AtEntry(*TrueBB, Id, 1)) {
          Instruction *IP = TrueBB->getFirstNonPHIOrDbgOrLifetime();
          if (IP) {
            IRBuilder<> B(IP);
            Value *PathV = ConstantInt::get(Type::getInt32Ty(Ctx), 1);
            EmitCheck(B, IdV, PathV);
            Changed = true;
          }
        }
        if (TrueBB && !hasExistingF2CountAtEntry(*TrueBB, Id, 1)) {
          Instruction *IP = TrueBB->getFirstNonPHIOrDbgOrLifetime();
          if (IP) {
            IRBuilder<> B(IP);
            Value *PathV = ConstantInt::get(Type::getInt32Ty(Ctx), 1);
            B.CreateCall(F2Cnt, {IdV, PathV});
            Changed = true;
          }
        }

        if (FalseBB && !hasExistingF2AtEntry(*FalseBB, Id, 0)) {
          Instruction *IP = FalseBB->getFirstNonPHIOrDbgOrLifetime();
          if (IP) {
            IRBuilder<> B(IP);
            Value *PathV = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
            EmitCheck(B, IdV, PathV);
            Changed = true;
          }
        }
        if (FalseBB && !hasExistingF2CountAtEntry(*FalseBB, Id, 0)) {
          Instruction *IP = FalseBB->getFirstNonPHIOrDbgOrLifetime();
          if (IP) {
            IRBuilder<> B(IP);
            Value *PathV = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
            B.CreateCall(F2Cnt, {IdV, PathV});
            Changed = true;
          }
        }
      } else {
        if (!TrueBB || !FalseBB) {
          // skip
        } else {
          Function &F = *Info.Func;
          auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
          auto &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);
          BasicBlock *JoinBB = PDT.findNearestCommonDominator(TrueBB, FalseBB);
          if (!JoinBB || isEhBlock(*JoinBB)) {
            errs() << "[intmon] F2 join 未找到或不可用 id=" << Id
                   << " func=" << F.getName() << "\n";
          } else {
            bool NeedF2 = !hasExistingF2AtEntryAnyPath(*JoinBB, Id);
            bool NeedF2Cnt = !hasExistingF2CountAtEntryAnyPath(*JoinBB, Id);
            if (!NeedF2 && !NeedF2Cnt) {
              // nothing to do
            } else {
              SmallVector<BasicBlock *, 8> TruePreds;
              SmallVector<BasicBlock *, 8> FalsePreds;
              if (!classifyJoinPreds(BI, TrueBB, FalseBB, JoinBB, DT,
                                     TruePreds, FalsePreds)) {
                errs() << "[intmon] F2 join 前驱不可判定 id=" << Id
                       << " func=" << F.getName() << "\n";
              } else if (Instruction *IP =
                             JoinBB->getFirstNonPHIOrDbgOrLifetime()) {
                PHINode *PathPhi = PHINode::Create(
                    Type::getInt32Ty(Ctx),
                    TruePreds.size() + FalsePreds.size(), "intmon_path", IP);
                for (BasicBlock *Pred : TruePreds) {
                  PathPhi->addIncoming(
                      ConstantInt::get(Type::getInt32Ty(Ctx), 1), Pred);
                }
                for (BasicBlock *Pred : FalsePreds) {
                  PathPhi->addIncoming(
                      ConstantInt::get(Type::getInt32Ty(Ctx), 0), Pred);
                }
                IRBuilder<> B(IP);
                if (NeedF2) {
                  EmitCheck(B, IdV, PathPhi);
                  Changed = true;
                }
                if (NeedF2Cnt) {
                  B.CreateCall(F2Cnt, {IdV, PathPhi});
                  Changed = true;
                }
              }
            }
          }
        }
      }

      if (BlockSecretInstr && TrueBB && FalseBB) {
        Function &F = *Info.Func;
        auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
        auto &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);
        BasicBlock *JoinBB = PDT.findNearestCommonDominator(TrueBB, FalseBB);
        if (!JoinBB || isEhBlock(*JoinBB)) {
          errs() << "[intmon] secret block 未找到 join id=" << Id
                 << " func=" << F.getName() << "\n";
        } else {
          DenseSet<BasicBlock *> TrueSet;
          DenseSet<BasicBlock *> FalseSet;
          BasicBlock *DomBB = BI->getParent();
          if (!collectReachableBlocks(TrueBB, JoinBB, DomBB, DT, TrueSet,
                                       DivergenceMaxNodes) ||
              !collectReachableBlocks(FalseBB, JoinBB, DomBB, DT, FalseSet,
                                       DivergenceMaxNodes)) {
            errs() << "[intmon] secret block CFG 过大或异常 id=" << Id
                   << " func=" << F.getName() << "\n";
          } else {
            for (BasicBlock &BB : F) {
              bool InT = TrueSet.contains(&BB);
              bool InF = FalseSet.contains(&BB);
              if (!InT && !InF) {
                continue;
              }
              if (&BB == JoinBB) {
                continue;
              }
              int32_t Path = 2;
              if (InT && !InF) {
                Path = 1;
              } else if (InF && !InT) {
                Path = 0;
              }
              Value *PathV = ConstantInt::get(Type::getInt32Ty(Ctx), Path);
              SmallVector<Instruction *, 8> Syscalls;
              for (Instruction &I : BB) {
                if (!isSyscallInstruction(I)) {
                  continue;
                }
                Syscalls.push_back(&I);
              }
              for (Instruction *I : Syscalls) {
                if (!hasExistingCheckBeforeInst(*I, Id, Path)) {
                  IRBuilder<> B(I);
                  EmitCheck(B, IdV, PathV);
                  Changed = true;
                }
                if (!hasExistingCheckCntBeforeInst(*I, Id, Path)) {
                  IRBuilder<> B(I);
                  B.CreateCall(F2Cnt, {IdV, PathV});
                  Changed = true;
                }
                Instruction *Next = I->getNextNode();
                if (!Next) {
                  continue;
                }
                if (!hasExistingPrepareBeforeInst(*Next, Id)) {
                  IRBuilder<> B(Next);
                  EmitPrepare(B, IdV);
                  Changed = true;
                }
                if (!hasExistingPrepareCntBeforeInst(*Next, Id)) {
                  IRBuilder<> B(Next);
                  B.CreateCall(F1Cnt, {IdV});
                  Changed = true;
                }
              }
            }
          }
        }
      }

      if (BranchSecretInstr && TrueBB && FalseBB) {
        Function &F = *Info.Func;
        auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
        auto &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);
        BasicBlock *JoinBB = PDT.findNearestCommonDominator(TrueBB, FalseBB);
        if (!JoinBB || isEhBlock(*JoinBB)) {
          errs() << "[intmon] secret branch 未找到 join id=" << Id
                 << " func=" << F.getName() << "\n";
        } else {
          DenseSet<BasicBlock *> TrueSet;
          DenseSet<BasicBlock *> FalseSet;
          BasicBlock *DomBB = BI->getParent();
          if (!collectReachableBlocks(TrueBB, JoinBB, DomBB, DT, TrueSet,
                                       DivergenceMaxNodes) ||
              !collectReachableBlocks(FalseBB, JoinBB, DomBB, DT, FalseSet,
                                       DivergenceMaxNodes)) {
            errs() << "[intmon] secret branch CFG 过大或异常 id=" << Id
                   << " func=" << F.getName() << "\n";
          } else {
            for (BasicBlock &BB : F) {
              bool InT = TrueSet.contains(&BB);
              bool InF = FalseSet.contains(&BB);
              if (!InT && !InF) {
                continue;
              }
              if (&BB == JoinBB) {
                continue;
              }
              int32_t Path = 2;
              if (InT && !InF) {
                Path = 1;
              } else if (InF && !InT) {
                Path = 0;
              }
              Value *PathV =
                  ConstantInt::get(Type::getInt32Ty(Ctx), Path);
              SmallVector<Instruction *, 32> Targets;
              for (Instruction &I : BB) {
                if (!shouldInstrumentInstruction(I)) {
                  continue;
                }
                Targets.push_back(&I);
              }
              for (Instruction *I : Targets) {
                if (!hasExistingPrepareBeforeInst(*I, Id)) {
                  IRBuilder<> B(I);
                  EmitPrepare(B, IdV);
                  Changed = true;
                }
                if (!hasExistingPrepareCntBeforeInst(*I, Id)) {
                  IRBuilder<> B(I);
                  B.CreateCall(F1Cnt, {IdV});
                  Changed = true;
                }
                Instruction *Next = I->getNextNode();
                if (!Next) {
                  continue;
                }
                if (!hasExistingCheckAfterInst(*I, Id, Path)) {
                  IRBuilder<> B(Next);
                  EmitCheck(B, IdV, PathV);
                  Changed = true;
                }
                if (!hasExistingCheckCntAfterInst(*I, Id, Path)) {
                  IRBuilder<> B(Next);
                  B.CreateCall(F2Cnt, {IdV, PathV});
                  Changed = true;
                }
              }
            }
          }
        }
      }
    }

    if (ShouldDiverge) {
      BasicBlock *TrueBB = BI->getSuccessor(0);
      BasicBlock *FalseBB = BI->getSuccessor(1);
      if (!TrueBB || !FalseBB) {
        continue;
      }

      if (!hasExistingDivAtEntry(*TrueBB, Id, 0)) {
        if (Instruction *IP = TrueBB->getFirstNonPHIOrDbgOrLifetime()) {
          IRBuilder<> B(IP);
          Value *Kind = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
          B.CreateCall(FDiv, {IdV, Kind});
          Changed = true;
          logDivPoint(Id, 0, *TrueBB, *Info.Func);
        }
      }

      if (!hasExistingDivAtEntry(*FalseBB, Id, 1)) {
        if (Instruction *IP = FalseBB->getFirstNonPHIOrDbgOrLifetime()) {
          IRBuilder<> B(IP);
          Value *Kind = ConstantInt::get(Type::getInt32Ty(Ctx), 1);
          B.CreateCall(FDiv, {IdV, Kind});
          Changed = true;
          logDivPoint(Id, 1, *FalseBB, *Info.Func);
        }
      }

      Function &F = *Info.Func;
      auto &PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);
      BasicBlock *JoinBB = PDT.findNearestCommonDominator(TrueBB, FalseBB);
      if (JoinBB && !isEhBlock(*JoinBB)) {
        if (!hasExistingDivAtEntry(*JoinBB, Id, 3)) {
          if (Instruction *IP = JoinBB->getFirstNonPHIOrDbgOrLifetime()) {
            IRBuilder<> B(IP);
            Value *Kind = ConstantInt::get(Type::getInt32Ty(Ctx), 3);
            B.CreateCall(FDiv, {IdV, Kind});
            Changed = true;
            logDivPoint(Id, 3, *JoinBB, *Info.Func);
          }
        }

        SmallVector<BasicBlock *, 32> PathT;
        SmallVector<BasicBlock *, 32> PathF;
        bool HasPathT = bfsFindPath(TrueBB, JoinBB, PathT, DivergenceMaxNodes);
        bool HasPathF = bfsFindPath(FalseBB, JoinBB, PathF, DivergenceMaxNodes);
        if (HasPathT && HasPathF) {
          DenseSet<BasicBlock *> SetT;
          DenseSet<BasicBlock *> SetF;
          for (BasicBlock *B : PathT) {
            SetT.insert(B);
          }
          for (BasicBlock *B : PathF) {
            SetF.insert(B);
          }

          for (BasicBlock *B : PathT) {
            if (B == TrueBB || B == FalseBB || B == JoinBB) {
              continue;
            }
            if (!SetF.contains(B) && !hasExistingDivAtEntry(*B, Id, 2)) {
              if (Instruction *IP = B->getFirstNonPHIOrDbgOrLifetime()) {
                IRBuilder<> IB(IP);
                Value *Kind = ConstantInt::get(Type::getInt32Ty(Ctx), 2);
                IB.CreateCall(FDiv, {IdV, Kind});
                Changed = true;
                logDivPoint(Id, 2, *B, *Info.Func);
              }
            }
          }

          for (BasicBlock *B : PathF) {
            if (B == TrueBB || B == FalseBB || B == JoinBB) {
              continue;
            }
            if (!SetT.contains(B) && !hasExistingDivAtEntry(*B, Id, 2)) {
              if (Instruction *IP = B->getFirstNonPHIOrDbgOrLifetime()) {
                IRBuilder<> IB(IP);
                Value *Kind = ConstantInt::get(Type::getInt32Ty(Ctx), 2);
                IB.CreateCall(FDiv, {IdV, Kind});
                Changed = true;
                logDivPoint(Id, 2, *B, *Info.Func);
              }
            }
          }
        }
      }
    }
  }

  }

  if (FuncInstr != FuncInstrumentMode::Off) {
    for (Function &F : M) {
      if (F.isDeclaration()) {
        continue;
      }
      if (FuncInstr == FuncInstrumentMode::Instrumented &&
          !InstrumentedFuncs.contains(&F)) {
        continue;
      }
      uint64_t FuncId = makeFunctionId(F);
      Value *FuncIdV = ConstantInt::get(Type::getInt64Ty(Ctx), FuncId);

      BasicBlock &Entry = F.getEntryBlock();
      Instruction *EntryIP = Entry.getFirstNonPHIOrDbgOrLifetime();
      if (EntryIP && !hasExistingFuncEnterAtEntry(Entry, FuncId)) {
        IRBuilder<> B(EntryIP);
        B.CreateCall(FEnter, {FuncIdV});
        Changed = true;
      }

      for (BasicBlock &BB : F) {
        Instruction *Term = BB.getTerminator();
        if (!Term) {
          continue;
        }
        if (isa<ReturnInst>(Term)) {
          if (!hasExistingFuncExitBefore(*Term, FuncId)) {
            IRBuilder<> B(Term);
            B.CreateCall(FExit, {FuncIdV});
            Changed = true;
          }
        }
      }
    }
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}  // namespace auto_instrument
