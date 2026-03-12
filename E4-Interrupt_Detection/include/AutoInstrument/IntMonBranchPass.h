#pragma once

#include "llvm/IR/PassManager.h"

namespace auto_instrument {

class IntMonBranchPass : public llvm::PassInfoMixin<IntMonBranchPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);
};

}  // namespace auto_instrument
