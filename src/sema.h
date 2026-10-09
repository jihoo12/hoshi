#pragma once

#include "ast.h"

namespace hoshi {

// Resolves names and types, annotating the AST in place. Throws CompileError
// on the first error.
void analyze(Module &m, TypeContext &types);

} // namespace hoshi
