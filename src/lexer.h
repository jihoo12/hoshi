#pragma once

#include "diag.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hoshi {

enum class Tok {
  Eof,
  Ident,
  IntLit,
  FloatLit,
  StringLit,
  CharLit,

  // Keywords
  KwFn, KwLet, KwVar, KwIf, KwElse, KwWhile, KwFor, KwIn, KwReturn, KwBreak,
  KwContinue, KwDefer, KwStruct, KwExtern, KwAs, KwTrue, KwFalse, KwNull,
  KwSizeof, KwConst, KwWhen,

  // Punctuation
  LParen, RParen, LBrace, RBrace, LBracket, RBracket,
  Comma, Semi, Colon, Dot, DotDot, Ellipsis, Arrow,

  // Operators
  Plus, Minus, Star, Slash, Percent, Amp, Pipe, Caret, Tilde, Bang,
  Shl, Shr, AmpAmp, PipePipe,
  EqEq, NotEq, Lt, LtEq, Gt, GtEq,
  Eq, PlusEq, MinusEq, StarEq, SlashEq, PercentEq, AmpEq, PipeEq, CaretEq,
  ShlEq, ShrEq,
};

const char *tokName(Tok t);

struct Token {
  Tok kind;
  SourceLoc loc;
  std::string text; // identifier name or decoded string literal
  uint64_t intVal = 0;
  double floatVal = 0;
};

std::vector<Token> lex(const SourceFile &file);

} // namespace hoshi
