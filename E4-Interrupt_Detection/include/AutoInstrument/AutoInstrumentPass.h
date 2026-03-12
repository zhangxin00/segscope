#pragma once

#include "llvm/IR/PassManager.h"

namespace auto_instrument {

class AutoInstrumentPass : public llvm::PassInfoMixin<AutoInstrumentPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);
};

}  // namespace auto_instrument
