#include "parser.h"

namespace hoshi {

namespace {

class Parser {
public:
  explicit Parser(std::vector<Token> tokens) : toks(std::move(tokens)) {}

  Module parseModule() {
    Module m;
    while (!at(Tok::Eof)) {
      if (at(Tok::KwStruct))
        m.structs.push_back(parseStruct());
      else if (at(Tok::KwFn) || at(Tok::KwExtern))
        m.funcs.push_back(parseFunc());
      else
        error(cur().loc, std::string("expected 'fn', 'extern' or 'struct', found ") +
                             tokName(cur().kind));
    }
    return m;
  }

private:
  std::vector<Token> toks;
  size_t pos = 0;
  // Inside `if`/`while`/`for` headers, `Name {` starts the body, not a struct
  // literal.
  bool noStructLit = false;

  const Token &cur() const { return toks[pos]; }
  const Token &peekTok(size_t off = 1) const {
    return toks[std::min(pos + off, toks.size() - 1)];
  }
  bool at(Tok k) const { return cur().kind == k; }
  Token next() {
    Token t = toks[pos];
    if (pos + 1 < toks.size())
      ++pos;
    return t;
  }
  bool accept(Tok k) {
    if (!at(k))
      return false;
    next();
    return true;
  }
  Token expect(Tok k, const char *context = nullptr) {
    if (!at(k)) {
      std::string msg = std::string("expected ") + tokName(k);
      if (context)
        msg += std::string(" ") + context;
      msg += std::string(", found ") + tokName(cur().kind);
      error(cur().loc, msg);
    }
    return next();
  }
  std::string expectIdent(const char *what) {
    if (!at(Tok::Ident))
      error(cur().loc, std::string("expected ") + what + ", found " +
                           tokName(cur().kind));
    return next().text;
  }

  // RAII helper to set noStructLit for a region.
  struct StructLitGuard {
    Parser &p;
    bool saved;
    StructLitGuard(Parser &p, bool value) : p(p), saved(p.noStructLit) {
      p.noStructLit = value;
    }
    ~StructLitGuard() { p.noStructLit = saved; }
  };

  // ---- Declarations -------------------------------------------------------

  std::unique_ptr<StructDecl> parseStruct() {
    auto s = std::make_unique<StructDecl>();
    s->loc = expect(Tok::KwStruct).loc;
    s->name = expectIdent("struct name");
    expect(Tok::LBrace, "after struct name");
    while (!at(Tok::RBrace)) {
      Field f;
      f.loc = cur().loc;
      f.name = expectIdent("field name");
      expect(Tok::Colon, "after field name");
      f.typeRef = parseType();
      s->fields.push_back(std::move(f));
      if (!accept(Tok::Comma))
        break;
    }
    expect(Tok::RBrace, "to close struct");
    return s;
  }

  std::unique_ptr<FuncDecl> parseFunc() {
    auto f = std::make_unique<FuncDecl>();
    f->isExtern = accept(Tok::KwExtern);
    f->loc = expect(Tok::KwFn).loc;
    f->name = expectIdent("function name");
    expect(Tok::LParen, "after function name");
    while (!at(Tok::RParen)) {
      if (at(Tok::Ellipsis)) {
        if (!f->isExtern)
          error(cur().loc, "only extern functions can be variadic");
        next();
        f->isVariadic = true;
        break;
      }
      Param p;
      p.var.loc = cur().loc;
      p.var.name = expectIdent("parameter name");
      expect(Tok::Colon, "after parameter name");
      p.typeRef = parseType();
      f->params.push_back(std::move(p));
      if (!accept(Tok::Comma))
        break;
    }
    expect(Tok::RParen, "to close parameter list");
    if (accept(Tok::Arrow))
      f->retRef = parseType();
    if (f->isExtern)
      expect(Tok::Semi, "after extern function declaration");
    else
      f->body = parseBlock();
    return f;
  }

  std::unique_ptr<TypeRef> parseType() {
    auto t = std::make_unique<TypeRef>();
    t->loc = cur().loc;
    if (accept(Tok::Star)) {
      t->kind = TypeRef::Pointer;
      t->elem = parseType();
    } else if (accept(Tok::LBracket)) {
      t->kind = TypeRef::Array;
      Token n = expect(Tok::IntLit, "for array length");
      t->count = n.intVal;
      expect(Tok::RBracket, "after array length");
      t->elem = parseType();
    } else {
      t->kind = TypeRef::Named;
      t->name = expectIdent("type");
    }
    return t;
  }

  // ---- Statements ---------------------------------------------------------

  std::unique_ptr<BlockStmt> parseBlock() {
    auto b = std::make_unique<BlockStmt>(cur().loc);
    expect(Tok::LBrace);
    StructLitGuard g(*this, false);
    while (!at(Tok::RBrace) && !at(Tok::Eof))
      b->stmts.push_back(parseStmt());
    expect(Tok::RBrace, "to close block");
    return b;
  }

  StmtPtr parseStmt() {
    SourceLoc loc = cur().loc;
    switch (cur().kind) {
    case Tok::LBrace:
      return parseBlock();
    case Tok::KwLet:
    case Tok::KwVar: {
      auto s = std::make_unique<LetStmt>(loc);
      s->var.isMutable = next().kind == Tok::KwVar;
      s->var.loc = cur().loc;
      s->var.name = expectIdent("variable name");
      if (accept(Tok::Colon))
        s->typeRef = parseType();
      if (accept(Tok::Eq))
        s->init = parseExpr();
      expect(Tok::Semi, "after variable declaration");
      return s;
    }
    case Tok::KwIf:
      return parseIf();
    case Tok::KwWhile: {
      next();
      auto s = std::make_unique<WhileStmt>(loc);
      {
        StructLitGuard g(*this, true);
        s->cond = parseExpr();
      }
      s->body = parseBlock();
      return s;
    }
    case Tok::KwFor: {
      next();
      auto s = std::make_unique<ForStmt>(loc);
      s->var.loc = cur().loc;
      s->var.name = expectIdent("loop variable name");
      expect(Tok::KwIn, "after loop variable");
      {
        StructLitGuard g(*this, true);
        s->start = parseExpr();
        expect(Tok::DotDot, "in for range");
        s->end = parseExpr();
      }
      s->body = parseBlock();
      return s;
    }
    case Tok::KwReturn: {
      next();
      ExprPtr value;
      if (!at(Tok::Semi))
        value = parseExpr();
      expect(Tok::Semi, "after return");
      return std::make_unique<ReturnStmt>(loc, std::move(value));
    }
    case Tok::KwBreak:
      next();
      expect(Tok::Semi, "after break");
      return std::make_unique<BreakStmt>(loc);
    case Tok::KwContinue:
      next();
      expect(Tok::Semi, "after continue");
      return std::make_unique<ContinueStmt>(loc);
    case Tok::KwDefer: {
      next();
      StmtPtr body = at(Tok::LBrace) ? parseBlock() : parseSimpleStmt();
      return std::make_unique<DeferStmt>(loc, std::move(body));
    }
    default:
      return parseSimpleStmt();
    }
  }

  StmtPtr parseIf() {
    auto s = std::make_unique<IfStmt>(expect(Tok::KwIf).loc);
    {
      StructLitGuard g(*this, true);
      s->cond = parseExpr();
    }
    s->thenBlock = parseBlock();
    if (accept(Tok::KwElse))
      s->elseStmt = at(Tok::KwIf) ? parseIf() : parseBlock();
    return s;
  }

  // Expression statement or assignment, terminated by ';'.
  StmtPtr parseSimpleStmt() {
    SourceLoc loc = cur().loc;
    ExprPtr lhs = parseExpr();

    static const std::pair<Tok, BinaryOp> compoundOps[] = {
        {Tok::PlusEq, BinaryOp::Add},    {Tok::MinusEq, BinaryOp::Sub},
        {Tok::StarEq, BinaryOp::Mul},    {Tok::SlashEq, BinaryOp::Div},
        {Tok::PercentEq, BinaryOp::Rem}, {Tok::AmpEq, BinaryOp::BitAnd},
        {Tok::PipeEq, BinaryOp::BitOr},  {Tok::CaretEq, BinaryOp::BitXor},
        {Tok::ShlEq, BinaryOp::Shl},     {Tok::ShrEq, BinaryOp::Shr},
    };

    StmtPtr result;
    if (at(Tok::Eq)) {
      auto a = std::make_unique<AssignStmt>(next().loc);
      a->target = std::move(lhs);
      a->value = parseExpr();
      result = std::move(a);
    } else {
      for (auto [tok, op] : compoundOps) {
        if (at(tok)) {
          auto a = std::make_unique<AssignStmt>(next().loc);
          a->compound = true;
          a->op = op;
          a->target = std::move(lhs);
          a->value = parseExpr();
          result = std::move(a);
          break;
        }
      }
      if (!result)
        result = std::make_unique<ExprStmt>(loc, std::move(lhs));
    }
    expect(Tok::Semi, "after statement");
    return result;
  }

  // ---- Expressions --------------------------------------------------------

  static int precedence(Tok k) {
    switch (k) {
    case Tok::PipePipe: return 1;
    case Tok::AmpAmp: return 2;
    case Tok::EqEq: case Tok::NotEq: case Tok::Lt: case Tok::LtEq:
    case Tok::Gt: case Tok::GtEq: return 3;
    case Tok::Pipe: return 4;
    case Tok::Caret: return 5;
    case Tok::Amp: return 6;
    case Tok::Shl: case Tok::Shr: return 7;
    case Tok::Plus: case Tok::Minus: return 8;
    case Tok::Star: case Tok::Slash: case Tok::Percent: return 9;
    case Tok::KwAs: return 10;
    default: return 0;
    }
  }

  static BinaryOp binaryOp(Tok k) {
    switch (k) {
    case Tok::PipePipe: return BinaryOp::Or;
    case Tok::AmpAmp: return BinaryOp::And;
    case Tok::EqEq: return BinaryOp::Eq;
    case Tok::NotEq: return BinaryOp::Ne;
    case Tok::Lt: return BinaryOp::Lt;
    case Tok::LtEq: return BinaryOp::Le;
    case Tok::Gt: return BinaryOp::Gt;
    case Tok::GtEq: return BinaryOp::Ge;
    case Tok::Pipe: return BinaryOp::BitOr;
    case Tok::Caret: return BinaryOp::BitXor;
    case Tok::Amp: return BinaryOp::BitAnd;
    case Tok::Shl: return BinaryOp::Shl;
    case Tok::Shr: return BinaryOp::Shr;
    case Tok::Plus: return BinaryOp::Add;
    case Tok::Minus: return BinaryOp::Sub;
    case Tok::Star: return BinaryOp::Mul;
    case Tok::Slash: return BinaryOp::Div;
    case Tok::Percent: return BinaryOp::Rem;
    default: break;
    }
    return BinaryOp::Add; // unreachable
  }

  ExprPtr parseExpr(int minPrec = 1) {
    ExprPtr lhs = parseUnary();
    for (;;) {
      int prec = precedence(cur().kind);
      if (prec < minPrec || prec == 0)
        return lhs;
      Token op = next();
      if (op.kind == Tok::KwAs) {
        lhs = std::make_unique<CastExpr>(op.loc, std::move(lhs), parseType());
        continue;
      }
      ExprPtr rhs = parseExpr(prec + 1);
      lhs = std::make_unique<BinaryExpr>(op.loc, binaryOp(op.kind),
                                         std::move(lhs), std::move(rhs));
    }
  }

  ExprPtr parseUnary() {
    SourceLoc loc = cur().loc;
    UnaryOp op;
    switch (cur().kind) {
    case Tok::Minus: op = UnaryOp::Neg; break;
    case Tok::Bang: op = UnaryOp::Not; break;
    case Tok::Tilde: op = UnaryOp::BitNot; break;
    case Tok::Amp: op = UnaryOp::AddrOf; break;
    case Tok::Star: op = UnaryOp::Deref; break;
    default: return parsePostfix(parsePrimary());
    }
    next();
    return std::make_unique<UnaryExpr>(loc, op, parseUnary());
  }

  ExprPtr parsePostfix(ExprPtr e) {
    for (;;) {
      SourceLoc loc = cur().loc;
      if (accept(Tok::Dot)) {
        std::string field = expectIdent("field name after '.'");
        e = std::make_unique<MemberExpr>(loc, std::move(e), std::move(field));
      } else if (accept(Tok::LBracket)) {
        StructLitGuard g(*this, false);
        ExprPtr index = parseExpr();
        expect(Tok::RBracket, "to close index");
        e = std::make_unique<IndexExpr>(loc, std::move(e), std::move(index));
      } else {
        return e;
      }
    }
  }

  ExprPtr parsePrimary() {
    Token t = cur();
    switch (t.kind) {
    case Tok::IntLit:
      next();
      return std::make_unique<IntLitExpr>(t.loc, t.intVal, false);
    case Tok::CharLit:
      next();
      return std::make_unique<IntLitExpr>(t.loc, t.intVal, true);
    case Tok::FloatLit:
      next();
      return std::make_unique<FloatLitExpr>(t.loc, t.floatVal);
    case Tok::StringLit: {
      next();
      std::string s = t.text;
      // Adjacent string literals concatenate, as in C.
      while (at(Tok::StringLit))
        s += next().text;
      return std::make_unique<StringLitExpr>(t.loc, std::move(s));
    }
    case Tok::KwTrue:
    case Tok::KwFalse:
      next();
      return std::make_unique<BoolLitExpr>(t.loc, t.kind == Tok::KwTrue);
    case Tok::KwNull:
      next();
      return std::make_unique<NullLitExpr>(t.loc);
    case Tok::KwSizeof: {
      next();
      expect(Tok::LParen, "after sizeof");
      auto ty = parseType();
      expect(Tok::RParen, "after sizeof type");
      return std::make_unique<SizeOfExpr>(t.loc, std::move(ty));
    }
    case Tok::LParen: {
      next();
      StructLitGuard g(*this, false);
      ExprPtr e = parseExpr();
      expect(Tok::RParen, "to close parenthesis");
      return e;
    }
    case Tok::LBracket: {
      next();
      StructLitGuard g(*this, false);
      std::vector<ExprPtr> elems;
      while (!at(Tok::RBracket)) {
        elems.push_back(parseExpr());
        if (!accept(Tok::Comma))
          break;
      }
      expect(Tok::RBracket, "to close array literal");
      return std::make_unique<ArrayLitExpr>(t.loc, std::move(elems));
    }
    case Tok::Ident: {
      next();
      if (at(Tok::LParen))
        return parseCall(t);
      if (at(Tok::LBrace) && !noStructLit)
        return parseStructLit(t);
      return std::make_unique<IdentExpr>(t.loc, t.text);
    }
    default:
      error(t.loc, std::string("expected expression, found ") + tokName(t.kind));
    }
  }

  ExprPtr parseCall(const Token &name) {
    expect(Tok::LParen);
    StructLitGuard g(*this, false);
    std::vector<ExprPtr> args;
    while (!at(Tok::RParen)) {
      args.push_back(parseExpr());
      if (!accept(Tok::Comma))
        break;
    }
    expect(Tok::RParen, "to close argument list");
    return std::make_unique<CallExpr>(name.loc, name.text, std::move(args));
  }

  ExprPtr parseStructLit(const Token &name) {
    expect(Tok::LBrace);
    std::vector<FieldInit> fields;
    while (!at(Tok::RBrace)) {
      FieldInit f;
      f.loc = cur().loc;
      f.name = expectIdent("field name");
      expect(Tok::Colon, "after field name");
      f.value = parseExpr();
      fields.push_back(std::move(f));
      if (!accept(Tok::Comma))
        break;
    }
    expect(Tok::RBrace, "to close struct literal");
    return std::make_unique<StructLitExpr>(name.loc, name.text, std::move(fields));
  }
};

} // namespace

Module parse(std::vector<Token> tokens) {
  return Parser(std::move(tokens)).parseModule();
}

} // namespace hoshi
