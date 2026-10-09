#pragma once

#include "ast.h"

#include <map>
#include <string>

namespace hoshi {

// Resolves names and types, evaluates compile-time constants and `when`,
// instantiates generics, and annotates the AST in place. `predefined` holds
// OS, ARCH and OPT_LEVEL; `overrides` holds -D values, which replace a global
// constant of the same name or else define a new one. Throws CompileError on
// the first error.
void analyze(Module &m, TypeContext &types, const std::map<std::string, ConstValue> &predefined,
             const std::map<std::string, ConstValue> &overrides);

} // namespace hoshi
