#pragma once

#include "ast.h"
#include "lexer.h"

namespace hoshi {

Module parse(std::vector<Token> tokens);

} // namespace hoshi
