#include "ast.h"

#include <cstdio>
#include <string>

namespace hoshi {

const char *binaryOpStr(BinaryOp op) {
  switch (op) {
  case BinaryOp::Add: return "+";
  case BinaryOp::Sub: return "-";
  case BinaryOp::Mul: return "*";
  case BinaryOp::Div: return "/";
  case BinaryOp::Rem: return "%";
  case BinaryOp::BitAnd: return "&";
  case BinaryOp::BitOr: return "|";
  case BinaryOp::BitXor: return "^";
  case BinaryOp::Shl: return "<<";
  case BinaryOp::Shr: return ">>";
  case BinaryOp::Eq: return "==";
  case BinaryOp::Ne: return "!=";
  case BinaryOp::Lt: return "<";
  case BinaryOp::Le: return "<=";
  case BinaryOp::Gt: return ">";
  case BinaryOp::Ge: return ">=";
  case BinaryOp::And: return "&&";
  case BinaryOp::Or: return "||";
  }
  return "?";
}

const char *unaryOpStr(UnaryOp op) {
  switch (op) {
  case UnaryOp::Neg: return "-";
  case UnaryOp::Not: return "!";
  case UnaryOp::BitNot: return "~";
  case UnaryOp::AddrOf: return "&";
  case UnaryOp::Deref: return "*";
  }
  return "?";
}

namespace {

std::string exprStr(const Expr *e);

std::string typeArgsStr(const std::vector<std::unique_ptr<TypeRef>> &args);

std::string typeRefStr(const TypeRef *t) {
  if (!t)
    return "void";
  switch (t->kind) {
  case TypeRef::Named: return t->name + typeArgsStr(t->args);
  case TypeRef::Pointer: return "*" + typeRefStr(t->elem.get());
  case TypeRef::Array:
    return "[" + exprStr(t->count.get()) + "]" + typeRefStr(t->elem.get());
  }
  return "?";
}

std::string typeArgsStr(const std::vector<std::unique_ptr<TypeRef>> &args) {
  if (args.empty())
    return "";
  std::string s = "[";
  for (size_t i = 0; i < args.size(); ++i)
    s += (i ? ", " : "") + typeRefStr(args[i].get());
  return s + "]";
}

std::string typeParamsStr(const std::vector<TypeParam> &params) {
  if (params.empty())
    return "";
  std::string s = "[";
  for (size_t i = 0; i < params.size(); ++i)
    s += (i ? ", " : "") + params[i].name;
  return s + "]";
}

std::string escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '\n': out += "\\n"; break;
    case '\t': out += "\\t"; break;
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    default: out += c;
    }
  }
  return out;
}

class Dumper {
public:
  void module(const Module &m) {
    for (auto &c : m.consts)
      constDecl(*c);
    for (auto &s : m.structs) {
      line("(struct " + s->name + typeParamsStr(s->typeParams));
      ++depth;
      for (auto &f : s->fields)
        line("(field " + f.name + " " + typeRefStr(f.typeRef.get()) + ")");
      --depth;
      line(")");
    }
    for (auto &f : m.funcs) {
      std::string head = std::string(f->isExtern ? "(extern-fn " : "(fn ") + f->name +
                         typeParamsStr(f->typeParams) + " (";
      for (size_t i = 0; i < f->params.size(); ++i) {
        if (i)
          head += " ";
        head += f->params[i].var.name + ":" + typeRefStr(f->params[i].typeRef.get());
      }
      if (f->isVariadic)
        head += f->params.empty() ? "..." : " ...";
      head += ") -> " + typeRefStr(f->retRef.get());
      if (!f->body) {
        line(head + ")");
        continue;
      }
      line(head);
      ++depth;
      stmt(f->body.get());
      --depth;
      line(")");
    }
    for (auto &w : m.whens) {
      line("(when " + expr(w->cond.get()));
      ++depth;
      module(w->thenItems);
      if (!w->elseItems.consts.empty() || !w->elseItems.structs.empty() ||
          !w->elseItems.funcs.empty() || !w->elseItems.whens.empty()) {
        line("(else");
        ++depth;
        module(w->elseItems);
        --depth;
        line(")");
      }
      --depth;
      line(")");
    }
  }

private:
  int depth = 0;

  void line(const std::string &s) {
    std::printf("%*s%s\n", depth * 2, "", s.c_str());
  }

  void constDecl(const ConstDecl &c) {
    std::string h = "(const " + c.name;
    if (c.typeRef)
      h += ":" + typeRefStr(c.typeRef.get());
    line(h + " = " + expr(c.init.get()) + ")");
  }

  void stmt(const Stmt *s) {
    switch (s->kind) {
    case StmtKind::Block: {
      line("(block");
      ++depth;
      for (auto &c : static_cast<const BlockStmt *>(s)->stmts)
        stmt(c.get());
      --depth;
      line(")");
      return;
    }
    case StmtKind::Const:
      constDecl(*static_cast<const ConstStmt *>(s)->decl);
      return;
    case StmtKind::When: {
      auto *w = static_cast<const WhenStmt *>(s);
      line("(when " + expr(w->cond.get()));
      ++depth;
      stmt(w->thenBlock.get());
      if (w->elseStmt)
        stmt(w->elseStmt.get());
      --depth;
      line(")");
      return;
    }
    case StmtKind::Let: {
      auto *l = static_cast<const LetStmt *>(s);
      std::string h = std::string(l->var.isMutable ? "(var " : "(let ") + l->var.name;
      if (l->typeRef)
        h += ":" + typeRefStr(l->typeRef.get());
      line(h + (l->init ? " = " + expr(l->init.get()) : "") + ")");
      return;
    }
    case StmtKind::Expr:
      line(expr(static_cast<const ExprStmt *>(s)->expr.get()));
      return;
    case StmtKind::Assign: {
      auto *a = static_cast<const AssignStmt *>(s);
      std::string op = a->compound ? std::string(binaryOpStr(a->op)) + "=" : "=";
      line("(" + op + " " + expr(a->target.get()) + " " + expr(a->value.get()) + ")");
      return;
    }
    case StmtKind::If: {
      auto *i = static_cast<const IfStmt *>(s);
      line("(if " + expr(i->cond.get()));
      ++depth;
      stmt(i->thenBlock.get());
      if (i->elseStmt)
        stmt(i->elseStmt.get());
      --depth;
      line(")");
      return;
    }
    case StmtKind::While: {
      auto *w = static_cast<const WhileStmt *>(s);
      line("(while " + expr(w->cond.get()));
      ++depth;
      stmt(w->body.get());
      --depth;
      line(")");
      return;
    }
    case StmtKind::For: {
      auto *f = static_cast<const ForStmt *>(s);
      line("(for " + f->var.name + " " + expr(f->start.get()) + " " + expr(f->end.get()));
      ++depth;
      stmt(f->body.get());
      --depth;
      line(")");
      return;
    }
    case StmtKind::Return: {
      auto *r = static_cast<const ReturnStmt *>(s);
      line(r->value ? "(return " + expr(r->value.get()) + ")" : "(return)");
      return;
    }
    case StmtKind::Break: line("(break)"); return;
    case StmtKind::Continue: line("(continue)"); return;
    case StmtKind::Defer:
      line("(defer");
      ++depth;
      stmt(static_cast<const DeferStmt *>(s)->body.get());
      --depth;
      line(")");
      return;
    }
  }

public:
  std::string expr(const Expr *e) {
    switch (e->kind) {
    case ExprKind::IntLit: {
      auto *i = static_cast<const IntLitExpr *>(e);
      return i->isChar ? "'" + escape(std::string(1, (char)i->value)) + "'"
                       : std::to_string(i->value);
    }
    case ExprKind::FloatLit: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%g", static_cast<const FloatLitExpr *>(e)->value);
      return buf;
    }
    case ExprKind::BoolLit:
      return static_cast<const BoolLitExpr *>(e)->value ? "true" : "false";
    case ExprKind::NullLit: return "null";
    case ExprKind::StringLit:
      return "\"" + escape(static_cast<const StringLitExpr *>(e)->value) + "\"";
    case ExprKind::Ident: return static_cast<const IdentExpr *>(e)->name;
    case ExprKind::Unary: {
      auto *u = static_cast<const UnaryExpr *>(e);
      return std::string("(") + unaryOpStr(u->op) + " " + expr(u->operand.get()) + ")";
    }
    case ExprKind::Binary: {
      auto *b = static_cast<const BinaryExpr *>(e);
      return std::string("(") + binaryOpStr(b->op) + " " + expr(b->lhs.get()) + " " +
             expr(b->rhs.get()) + ")";
    }
    case ExprKind::Call: {
      auto *c = static_cast<const CallExpr *>(e);
      std::string s = "(call " + c->callee + typeArgsStr(c->typeArgs);
      for (auto &a : c->args)
        s += " " + expr(a.get());
      return s + ")";
    }
    case ExprKind::Member: {
      auto *m = static_cast<const MemberExpr *>(e);
      return "(. " + expr(m->base.get()) + " " + m->field + ")";
    }
    case ExprKind::Index: {
      auto *i = static_cast<const IndexExpr *>(e);
      return "([] " + expr(i->base.get()) + " " + expr(i->index.get()) + ")";
    }
    case ExprKind::Cast: {
      auto *c = static_cast<const CastExpr *>(e);
      return "(as " + expr(c->operand.get()) + " " + typeRefStr(c->target.get()) + ")";
    }
    case ExprKind::SizeOf:
      return "(sizeof " + typeRefStr(static_cast<const SizeOfExpr *>(e)->target.get()) + ")";
    case ExprKind::StructLit: {
      auto *s = static_cast<const StructLitExpr *>(e);
      std::string out = "(struct-lit " + s->name + typeArgsStr(s->typeArgs);
      for (auto &f : s->fields)
        out += " " + f.name + ":" + expr(f.value.get());
      return out + ")";
    }
    case ExprKind::ArrayLit: {
      auto *a = static_cast<const ArrayLitExpr *>(e);
      std::string out = "(array";
      for (auto &el : a->elems)
        out += " " + expr(el.get());
      return out + ")";
    }
    }
    return "?";
  }
};

std::string exprStr(const Expr *e) { return e ? Dumper().expr(e) : "?"; }

} // namespace

std::string ConstValue::str() const {
  switch (kind) {
  case Int: {
    if (i == 0)
      return "0";
    bool neg = i < 0;
    unsigned __int128 v = neg ? -(unsigned __int128)i : (unsigned __int128)i;
    std::string s;
    while (v) {
      s.insert(s.begin(), char('0' + int(v % 10)));
      v /= 10;
    }
    return neg ? "-" + s : s;
  }
  case Float: {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", f);
    return buf;
  }
  case Bool: return b ? "true" : "false";
  case Str: return "\"" + escape(s) + "\"";
  }
  return "?";
}

void dumpModule(const Module &m) { Dumper().module(m); }

} // namespace hoshi
