#pragma once

#include "ast.h"

namespace llvm {
class Module;
}

namespace hoshi {

// Emits IR for an analyzed module into `out`, whose target triple and data
// layout must already be set.
void generate(Module &m, TypeContext &types, llvm::Module &out);

} // namespace hoshi
