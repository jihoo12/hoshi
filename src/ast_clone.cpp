#include "ast.h"

namespace hoshi {

namespace {

VarSym cloneVar(const VarSym &v) {
  VarSym out;
  out.name = v.name;
  out.loc = v.loc;
  out.isMutable = v.isMutable;
  return out;
}

std::vector<std::unique_ptr<TypeRef>>
cloneTypeRefs(const std::vector<std::unique_ptr<TypeRef>> &ts) {
  std::vector<std::unique_ptr<TypeRef>> out;
  for (auto &t : ts)
    out.push_back(cloneTypeRef(*t));
  return out;
}

std::unique_ptr<TypeRef> cloneOptTypeRef(const std::unique_ptr<TypeRef> &t) {
  return t ? cloneTypeRef(*t) : nullptr;
}

ExprPtr cloneOptExpr(const ExprPtr &e) { return e ? cloneExpr(*e) : nullptr; }

std::unique_ptr<BlockStmt> cloneBlock(const BlockStmt &b) {
  auto out = std::make_unique<BlockStmt>(b.loc);
  for (auto &s : b.stmts)
    out->stmts.push_back(cloneStmt(*s));
  return out;
}

std::unique_ptr<ConstDecl> cloneConst(const ConstDecl &c) {
  auto out = std::make_unique<ConstDecl>();
  out->name = c.name;
  out->loc = c.loc;
  out->typeRef = cloneOptTypeRef(c.typeRef);
  out->init = cloneOptExpr(c.init);
  return out;
}

} // namespace

std::unique_ptr<TypeRef> cloneTypeRef(const TypeRef &t) {
  auto out = std::make_unique<TypeRef>();
  out->kind = t.kind;
  out->loc = t.loc;
  out->name = t.name;
  out->args = cloneTypeRefs(t.args);
  out->elem = cloneOptTypeRef(t.elem);
  out->count = cloneOptExpr(t.count);
  return out;
}

ExprPtr cloneExpr(const Expr &e) {
  switch (e.kind) {
  case ExprKind::IntLit: {
    auto &i = static_cast<const IntLitExpr &>(e);
    return std::make_unique<IntLitExpr>(i.loc, i.value, i.isChar);
  }
  case ExprKind::FloatLit:
    return std::make_unique<FloatLitExpr>(e.loc, static_cast<const FloatLitExpr &>(e).value);
  case ExprKind::BoolLit:
    return std::make_unique<BoolLitExpr>(e.loc, static_cast<const BoolLitExpr &>(e).value);
  case ExprKind::NullLit:
    return std::make_unique<NullLitExpr>(e.loc);
  case ExprKind::StringLit:
    return std::make_unique<StringLitExpr>(e.loc,
                                           static_cast<const StringLitExpr &>(e).value);
  case ExprKind::Ident:
    return std::make_unique<IdentExpr>(e.loc, static_cast<const IdentExpr &>(e).name);
  case ExprKind::Unary: {
    auto &u = static_cast<const UnaryExpr &>(e);
    return std::make_unique<UnaryExpr>(u.loc, u.op, cloneExpr(*u.operand));
  }
  case ExprKind::Binary: {
    auto &b = static_cast<const BinaryExpr &>(e);
    return std::make_unique<BinaryExpr>(b.loc, b.op, cloneExpr(*b.lhs), cloneExpr(*b.rhs));
  }
  case ExprKind::Call: {
    auto &c = static_cast<const CallExpr &>(e);
    std::vector<ExprPtr> args;
    for (auto &a : c.args)
      args.push_back(cloneExpr(*a));
    auto out = std::make_unique<CallExpr>(c.loc, c.callee, std::move(args));
    out->typeArgs = cloneTypeRefs(c.typeArgs);
    return out;
  }
  case ExprKind::Member: {
    auto &m = static_cast<const MemberExpr &>(e);
    return std::make_unique<MemberExpr>(m.loc, cloneExpr(*m.base), m.field);
  }
  case ExprKind::Index: {
    auto &i = static_cast<const IndexExpr &>(e);
    return std::make_unique<IndexExpr>(i.loc, cloneExpr(*i.base), cloneExpr(*i.index));
  }
  case ExprKind::Cast: {
    auto &c = static_cast<const CastExpr &>(e);
    return std::make_unique<CastExpr>(c.loc, cloneExpr(*c.operand), cloneTypeRef(*c.target));
  }
  case ExprKind::SizeOf:
    return std::make_unique<SizeOfExpr>(
        e.loc, cloneTypeRef(*static_cast<const SizeOfExpr &>(e).target));
  case ExprKind::StructLit: {
    auto &s = static_cast<const StructLitExpr &>(e);
    std::vector<FieldInit> fields;
    for (auto &f : s.fields)
      fields.push_back(FieldInit{f.name, f.loc, cloneExpr(*f.value)});
    auto out = std::make_unique<StructLitExpr>(s.loc, s.name, std::move(fields));
    out->typeArgs = cloneTypeRefs(s.typeArgs);
    return out;
  }
  case ExprKind::ArrayLit: {
    auto &a = static_cast<const ArrayLitExpr &>(e);
    std::vector<ExprPtr> elems;
    for (auto &el : a.elems)
      elems.push_back(cloneExpr(*el));
    return std::make_unique<ArrayLitExpr>(a.loc, std::move(elems));
  }
  }
  return nullptr;
}

StmtPtr cloneStmt(const Stmt &s) {
  switch (s.kind) {
  case StmtKind::Block:
    return cloneBlock(static_cast<const BlockStmt &>(s));
  case StmtKind::Let: {
    auto &l = static_cast<const LetStmt &>(s);
    auto out = std::make_unique<LetStmt>(l.loc);
    out->var = cloneVar(l.var);
    out->typeRef = cloneOptTypeRef(l.typeRef);
    out->init = cloneOptExpr(l.init);
    return out;
  }
  case StmtKind::Const: {
    auto &c = static_cast<const ConstStmt &>(s);
    return std::make_unique<ConstStmt>(c.loc, cloneConst(*c.decl));
  }
  case StmtKind::Expr:
    return std::make_unique<ExprStmt>(s.loc, cloneExpr(*static_cast<const ExprStmt &>(s).expr));
  case StmtKind::Assign: {
    auto &a = static_cast<const AssignStmt &>(s);
    auto out = std::make_unique<AssignStmt>(a.loc);
    out->compound = a.compound;
    out->op = a.op;
    out->target = cloneExpr(*a.target);
    out->value = cloneExpr(*a.value);
    return out;
  }
  case StmtKind::If: {
    auto &i = static_cast<const IfStmt &>(s);
    auto out = std::make_unique<IfStmt>(i.loc);
    out->cond = cloneExpr(*i.cond);
    out->thenBlock = cloneBlock(*i.thenBlock);
    if (i.elseStmt)
      out->elseStmt = cloneStmt(*i.elseStmt);
    return out;
  }
  case StmtKind::When: {
    auto &w = static_cast<const WhenStmt &>(s);
    auto out = std::make_unique<WhenStmt>(w.loc);
    out->cond = cloneExpr(*w.cond);
    out->thenBlock = cloneBlock(*w.thenBlock);
    if (w.elseStmt)
      out->elseStmt = cloneStmt(*w.elseStmt);
    return out;
  }
  case StmtKind::While: {
    auto &w = static_cast<const WhileStmt &>(s);
    auto out = std::make_unique<WhileStmt>(w.loc);
    out->cond = cloneExpr(*w.cond);
    out->body = cloneBlock(*w.body);
    return out;
  }
  case StmtKind::For: {
    auto &f = static_cast<const ForStmt &>(s);
    auto out = std::make_unique<ForStmt>(f.loc);
    out->var = cloneVar(f.var);
    out->start = cloneExpr(*f.start);
    out->end = cloneExpr(*f.end);
    out->body = cloneBlock(*f.body);
    return out;
  }
  case StmtKind::Return:
    return std::make_unique<ReturnStmt>(
        s.loc, cloneOptExpr(static_cast<const ReturnStmt &>(s).value));
  case StmtKind::Break:
    return std::make_unique<BreakStmt>(s.loc);
  case StmtKind::Continue:
    return std::make_unique<ContinueStmt>(s.loc);
  case StmtKind::Defer:
    return std::make_unique<DeferStmt>(s.loc,
                                       cloneStmt(*static_cast<const DeferStmt &>(s).body));
  }
  return nullptr;
}

} // namespace hoshi
