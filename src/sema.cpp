#include "sema.h"

#include <unordered_map>

namespace hoshi {

namespace {

class Sema {
public:
  Sema(Module &m, TypeContext &tc) : m(m), tc(tc) {}

  void run() {
    for (auto &s : m.structs) {
      if (tc.builtin(s->name))
        error(s->loc, "'" + s->name + "' is a builtin type name");
      if (!structs.emplace(s->name, s.get()).second)
        error(s->loc, "redefinition of struct '" + s->name + "'");
      s->type = tc.structTy(s.get());
    }
    for (auto &s : m.structs)
      resolveFields(*s);
    for (auto &s : m.structs)
      checkStructCycle(s.get());

    for (auto &f : m.funcs)
      declareFunc(*f);
    for (auto &f : m.funcs)
      if (f->body)
        checkFuncBody(*f);
  }

private:
  Module &m;
  TypeContext &tc;
  std::unordered_map<std::string, StructDecl *> structs;
  std::unordered_map<std::string, FuncDecl *> funcs;
  std::vector<std::unordered_map<std::string, VarSym *>> scopes;
  FuncDecl *curFn = nullptr;
  int loopDepth = 0;
  int deferDepth = 0;

  // ---- Types --------------------------------------------------------------

  Type *resolveType(TypeRef &t) {
    switch (t.kind) {
    case TypeRef::Named:
      if (Type *b = tc.builtin(t.name))
        t.resolved = b;
      else if (auto it = structs.find(t.name); it != structs.end())
        t.resolved = it->second->type;
      else
        error(t.loc, "unknown type '" + t.name + "'");
      break;
    case TypeRef::Pointer:
      t.resolved = tc.pointerTo(resolveType(*t.elem));
      break;
    case TypeRef::Array: {
      Type *elem = resolveType(*t.elem);
      if (elem->isVoid())
        error(t.loc, "array element type cannot be void");
      if (t.count == 0)
        error(t.loc, "array length must be greater than zero");
      t.resolved = tc.arrayOf(elem, t.count);
      break;
    }
    }
    return t.resolved;
  }

  Type *resolveValueType(TypeRef &t, const char *what) {
    Type *ty = resolveType(t);
    if (ty->isVoid())
      error(t.loc, std::string(what) + " cannot have type void");
    return ty;
  }

  void resolveFields(StructDecl &s) {
    for (size_t i = 0; i < s.fields.size(); ++i) {
      Field &f = s.fields[i];
      for (size_t j = 0; j < i; ++j)
        if (s.fields[j].name == f.name)
          error(f.loc, "duplicate field '" + f.name + "'");
      f.type = resolveValueType(*f.typeRef, "a field");
    }
  }

  // A struct that contains itself by value would have infinite size.
  std::unordered_map<StructDecl *, int> cycleState; // 1 = visiting, 2 = done
  void checkStructCycle(StructDecl *s) {
    int &st = cycleState[s];
    if (st == 2)
      return;
    if (st == 1)
      error(s->loc, "struct '" + s->name +
                        "' contains itself by value; use a pointer instead");
    st = 1;
    for (auto &f : s->fields) {
      Type *t = f.type;
      while (t->isArray())
        t = t->elem;
      if (t->isStruct())
        checkStructCycle(t->decl);
    }
    cycleState[s] = 2;
  }

  // ---- Functions ----------------------------------------------------------

  void declareFunc(FuncDecl &f) {
    if (!funcs.emplace(f.name, &f).second)
      error(f.loc, "redefinition of function '" + f.name + "'");
    for (auto &p : f.params) {
      p.var.type = resolveValueType(*p.typeRef, "a parameter");
      p.var.isMutable = false;
    }
    f.retType = f.retRef ? resolveType(*f.retRef) : tc.voidTy();

    // Aggregates cross the C boundary with target-specific ABI rules (register
    // splitting, sret, byval) that codegen does not implement yet.
    if (f.isExtern) {
      for (auto &p : f.params)
        if (p.var.type->isStruct() || p.var.type->isArray())
          error(p.typeRef->loc, "extern functions cannot take " + p.var.type->str() +
                                    " by value; pass a pointer instead");
      if (f.retType->isStruct() || f.retType->isArray())
        error(f.retRef->loc, "extern functions cannot return " + f.retType->str() +
                                 " by value; return it through a pointer instead");
    }

    if (f.name == "main" && !f.isExtern) {
      Type *i32 = tc.intTy(32, true);
      if (f.retType != i32 && !f.retType->isVoid())
        error(f.loc, "'main' must return i32 or nothing");
      bool argsOk = f.params.empty() ||
                    (f.params.size() == 2 && f.params[0].var.type == i32 &&
                     f.params[1].var.type ==
                         tc.pointerTo(tc.pointerTo(tc.intTy(8, false))));
      if (!argsOk)
        error(f.loc, "'main' must take no parameters or (argc: i32, argv: **u8)");
    }
  }

  void checkFuncBody(FuncDecl &f) {
    curFn = &f;
    scopes.clear();
    scopes.emplace_back();
    for (auto &p : f.params) {
      if (scopes.back().count(p.var.name))
        error(p.var.loc, "duplicate parameter '" + p.var.name + "'");
      scopes.back()[p.var.name] = &p.var;
    }
    checkBlock(*f.body);
    scopes.clear();

    bool implicitReturnOk = f.retType->isVoid() || f.name == "main";
    if (!implicitReturnOk && !alwaysReturns(*f.body))
      error(f.loc, "function '" + f.name + "' does not return a value on every path");
    curFn = nullptr;
  }

  static bool containsBreak(const Stmt &s) {
    switch (s.kind) {
    case StmtKind::Break: return true;
    case StmtKind::Block:
      for (auto &c : static_cast<const BlockStmt &>(s).stmts)
        if (containsBreak(*c))
          return true;
      return false;
    case StmtKind::If: {
      auto &i = static_cast<const IfStmt &>(s);
      return containsBreak(*i.thenBlock) || (i.elseStmt && containsBreak(*i.elseStmt));
    }
    default: return false; // nested loops own their breaks
    }
  }

  static bool alwaysReturns(const Stmt &s) {
    switch (s.kind) {
    case StmtKind::Return: return true;
    case StmtKind::Block:
      for (auto &c : static_cast<const BlockStmt &>(s).stmts)
        if (alwaysReturns(*c))
          return true;
      return false;
    case StmtKind::If: {
      auto &i = static_cast<const IfStmt &>(s);
      return i.elseStmt && alwaysReturns(*i.thenBlock) && alwaysReturns(*i.elseStmt);
    }
    case StmtKind::While: {
      // `while true { ... }` without a break never falls through.
      auto &w = static_cast<const WhileStmt &>(s);
      return w.cond->kind == ExprKind::BoolLit &&
             static_cast<const BoolLitExpr &>(*w.cond).value && !containsBreak(*w.body);
    }
    default: return false;
    }
  }

  // ---- Scopes -------------------------------------------------------------

  void declareVar(VarSym &v) { scopes.back()[v.name] = &v; }

  VarSym *lookupVar(const std::string &name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
      if (auto f = it->find(name); f != it->end())
        return f->second;
    return nullptr;
  }

  // ---- Statements ---------------------------------------------------------

  void checkBlock(BlockStmt &b) {
    scopes.emplace_back();
    for (auto &s : b.stmts)
      checkStmt(*s);
    scopes.pop_back();
  }

  void checkStmt(Stmt &s) {
    switch (s.kind) {
    case StmtKind::Block:
      checkBlock(static_cast<BlockStmt &>(s));
      return;
    case StmtKind::Let: {
      auto &l = static_cast<LetStmt &>(s);
      Type *declared = l.typeRef ? resolveValueType(*l.typeRef, "a variable") : nullptr;
      if (l.init) {
        Type *t = checkExpr(*l.init, declared);
        if (declared)
          requireType(*l.init, declared);
        else if (t->isVoid())
          error(l.init->loc, "cannot initialize a variable with a void value");
        l.var.type = declared ? declared : t;
      } else {
        if (!l.var.isMutable)
          error(l.loc, "'let' variable '" + l.var.name +
                           "' needs an initializer (use 'var' for a zeroed variable)");
        if (!declared)
          error(l.loc, "variable '" + l.var.name + "' needs a type or an initializer");
        l.var.type = declared;
      }
      declareVar(l.var);
      return;
    }
    case StmtKind::Expr:
      checkExpr(*static_cast<ExprStmt &>(s).expr, nullptr);
      return;
    case StmtKind::Assign: {
      auto &a = static_cast<AssignStmt &>(s);
      Type *target = checkExpr(*a.target, nullptr);
      checkMutable(*a.target);
      checkExpr(*a.value, a.compound && target->isPointer() ? nullptr : target);
      if (!a.compound)
        requireType(*a.value, target);
      else if (binaryResultType(a.op, *a.target, *a.value, a.loc) != target)
        error(a.loc, "compound assignment result does not match the target type");
      return;
    }
    case StmtKind::If: {
      auto &i = static_cast<IfStmt &>(s);
      checkCond(*i.cond);
      checkBlock(*i.thenBlock);
      if (i.elseStmt)
        checkStmt(*i.elseStmt);
      return;
    }
    case StmtKind::While: {
      auto &w = static_cast<WhileStmt &>(s);
      checkCond(*w.cond);
      ++loopDepth;
      checkBlock(*w.body);
      --loopDepth;
      return;
    }
    case StmtKind::For: {
      auto &f = static_cast<ForStmt &>(s);
      checkOperands(*f.start, *f.end, nullptr);
      if (f.start->type != f.end->type)
        error(f.end->loc, "range bounds have different types: " +
                              f.start->type->str() + " and " + f.end->type->str());
      if (!f.start->type->isInt())
        error(f.start->loc, "range bounds must be integers, found " + f.start->type->str());
      f.var.type = f.start->type;
      f.var.isMutable = false;
      scopes.emplace_back();
      declareVar(f.var);
      ++loopDepth;
      checkBlock(*f.body);
      --loopDepth;
      scopes.pop_back();
      return;
    }
    case StmtKind::Return: {
      auto &r = static_cast<ReturnStmt &>(s);
      if (deferDepth > 0)
        error(r.loc, "cannot return from inside a defer");
      Type *ret = curFn->retType;
      if (r.value) {
        if (ret->isVoid())
          error(r.value->loc, "function '" + curFn->name + "' does not return a value");
        checkExpr(*r.value, ret);
        requireType(*r.value, ret);
      } else if (!ret->isVoid()) {
        error(r.loc, "missing return value of type " + ret->str());
      }
      return;
    }
    case StmtKind::Break:
    case StmtKind::Continue:
      if (loopDepth == 0)
        error(s.loc, std::string(s.kind == StmtKind::Break ? "'break'" : "'continue'") +
                         " outside of a loop");
      return;
    case StmtKind::Defer: {
      auto &d = static_cast<DeferStmt &>(s);
      if (deferDepth > 0)
        error(d.loc, "defer cannot appear inside another defer");
      int savedLoop = loopDepth;
      loopDepth = 0; // break/continue cannot jump out of a defer
      ++deferDepth;
      checkStmt(*d.body);
      --deferDepth;
      loopDepth = savedLoop;
      return;
    }
    }
  }

  void checkCond(Expr &e) {
    Type *t = checkExpr(e, tc.boolTy());
    if (!t->isBool())
      error(e.loc, "condition must be bool, found " + t->str());
  }

  // ---- Expressions --------------------------------------------------------

  void requireType(Expr &e, Type *want) {
    if (e.type != want)
      error(e.loc, "expected " + want->str() + ", found " + e.type->str());
  }

  // Literals whose type is decided by context.
  static bool isUntyped(const Expr &e) {
    switch (e.kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::NullLit:
      return true;
    case ExprKind::Unary: {
      auto &u = static_cast<const UnaryExpr &>(e);
      return (u.op == UnaryOp::Neg || u.op == UnaryOp::BitNot) && isUntyped(*u.operand);
    }
    case ExprKind::Binary: {
      auto &b = static_cast<const BinaryExpr &>(e);
      switch (b.op) {
      case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt: case BinaryOp::Le:
      case BinaryOp::Gt: case BinaryOp::Ge: case BinaryOp::And: case BinaryOp::Or:
        return false;
      default:
        return isUntyped(*b.lhs) && isUntyped(*b.rhs);
      }
    }
    default:
      return false;
    }
  }

  // Check two operands that must agree in type: a literal side adopts the
  // type of the other side.
  void checkOperands(Expr &lhs, Expr &rhs, Type *expected) {
    if (isUntyped(lhs) && !isUntyped(rhs)) {
      checkExpr(rhs, expected);
      checkExpr(lhs, rhs.type->isPointer() && lhs.kind != ExprKind::NullLit
                         ? nullptr : rhs.type);
    } else {
      checkExpr(lhs, expected);
      checkExpr(rhs, lhs.type->isPointer() && rhs.kind != ExprKind::NullLit
                         ? nullptr : lhs.type);
    }
  }

  static bool fitsInt(uint64_t v, Type *t, bool negated) {
    if (t->isSigned) {
      uint64_t max = (uint64_t(1) << (t->bits - 1)) - 1;
      return v <= max + (negated ? 1 : 0);
    }
    if (negated)
      return v == 0;
    return t->bits == 64 || v <= (uint64_t(1) << t->bits) - 1;
  }

  Type *checkIntLit(IntLitExpr &e, Type *expected, bool negated) {
    Type *t;
    if (expected && expected->isNumeric())
      t = expected;
    else if (e.isChar)
      t = tc.intTy(8, false);
    else
      t = fitsInt(e.value, tc.intTy(32, true), negated) ? tc.intTy(32, true)
                                                        : tc.intTy(64, true);
    if (t->isInt() && !fitsInt(e.value, t, negated))
      error(e.loc, "integer literal " + std::string(negated ? "-" : "") +
                       std::to_string(e.value) + " does not fit in " + t->str());
    return e.type = t;
  }

  Type *checkExpr(Expr &e, Type *expected) {
    switch (e.kind) {
    case ExprKind::IntLit:
      return checkIntLit(static_cast<IntLitExpr &>(e), expected, false);
    case ExprKind::FloatLit:
      return e.type = expected && expected->isFloat() ? expected : tc.floatTy(64);
    case ExprKind::BoolLit:
      return e.type = tc.boolTy();
    case ExprKind::NullLit:
      return e.type = expected && expected->isPointer() ? expected
                                                        : tc.pointerTo(tc.voidTy());
    case ExprKind::StringLit:
      return e.type = tc.pointerTo(tc.intTy(8, false));
    case ExprKind::Ident: {
      auto &id = static_cast<IdentExpr &>(e);
      id.var = lookupVar(id.name);
      if (!id.var) {
        if (funcs.count(id.name))
          error(e.loc, "function '" + id.name + "' cannot be used as a value");
        error(e.loc, "use of undeclared variable '" + id.name + "'");
      }
      return e.type = id.var->type;
    }
    case ExprKind::Unary:
      return checkUnary(static_cast<UnaryExpr &>(e), expected);
    case ExprKind::Binary: {
      auto &b = static_cast<BinaryExpr &>(e);
      bool isCmp = b.op == BinaryOp::Eq || b.op == BinaryOp::Ne || b.op == BinaryOp::Lt ||
                   b.op == BinaryOp::Le || b.op == BinaryOp::Gt || b.op == BinaryOp::Ge;
      if (b.op == BinaryOp::And || b.op == BinaryOp::Or) {
        checkExpr(*b.lhs, tc.boolTy());
        checkExpr(*b.rhs, tc.boolTy());
      } else {
        checkOperands(*b.lhs, *b.rhs, isCmp ? nullptr : expected);
      }
      return e.type = binaryResultType(b.op, *b.lhs, *b.rhs, b.loc);
    }
    case ExprKind::Call:
      return checkCall(static_cast<CallExpr &>(e));
    case ExprKind::Member: {
      auto &me = static_cast<MemberExpr &>(e);
      Type *base = checkExpr(*me.base, nullptr);
      if (base->isPointer() && base->elem->isStruct()) {
        me.throughPointer = true;
        base = base->elem;
      }
      if (!base->isStruct())
        error(me.loc, "cannot access field '" + me.field + "' on type " + base->str());
      int idx = base->decl->fieldIndex(me.field);
      if (idx < 0)
        error(me.loc, "struct '" + base->decl->name + "' has no field '" + me.field + "'");
      me.fieldIndex = idx;
      return e.type = base->decl->fields[idx].type;
    }
    case ExprKind::Index: {
      auto &ix = static_cast<IndexExpr &>(e);
      Type *base = checkExpr(*ix.base, nullptr);
      if (!base->isArray() && !base->isPointer())
        error(ix.loc, "cannot index into a value of type " + base->str());
      if (base->elem->isVoid())
        error(ix.loc, "cannot index through *void; cast it to a typed pointer first");
      Type *idx = checkExpr(*ix.index, nullptr);
      if (!idx->isInt())
        error(ix.index->loc, "index must be an integer, found " + idx->str());
      return e.type = base->elem;
    }
    case ExprKind::Cast: {
      auto &c = static_cast<CastExpr &>(e);
      Type *to = resolveType(*c.target);
      Type *from = checkExpr(*c.operand, nullptr);
      if (!castAllowed(from, to))
        error(c.loc, "cannot cast " + from->str() + " to " + to->str());
      return e.type = to;
    }
    case ExprKind::SizeOf: {
      auto &so = static_cast<SizeOfExpr &>(e);
      resolveValueType(*so.target, "sizeof operand");
      return e.type = tc.intTy(64, false);
    }
    case ExprKind::StructLit:
      return checkStructLit(static_cast<StructLitExpr &>(e));
    case ExprKind::ArrayLit:
      return checkArrayLit(static_cast<ArrayLitExpr &>(e), expected);
    }
    return nullptr;
  }

  Type *checkUnary(UnaryExpr &u, Type *expected) {
    switch (u.op) {
    case UnaryOp::Neg: {
      Type *t;
      if (u.operand->kind == ExprKind::IntLit)
        t = checkIntLit(static_cast<IntLitExpr &>(*u.operand), expected, true);
      else
        t = checkExpr(*u.operand, expected);
      if (!(t->isFloat() || (t->isInt() && t->isSigned)))
        error(u.loc, "cannot negate a value of type " + t->str());
      return u.type = t;
    }
    case UnaryOp::Not: {
      Type *t = checkExpr(*u.operand, tc.boolTy());
      if (!t->isBool())
        error(u.loc, "'!' needs a bool operand, found " + t->str() +
                         " (use '~' for bitwise not)");
      return u.type = t;
    }
    case UnaryOp::BitNot: {
      Type *t = checkExpr(*u.operand, expected);
      if (!t->isInt())
        error(u.loc, "'~' needs an integer operand, found " + t->str());
      return u.type = t;
    }
    case UnaryOp::AddrOf: {
      Type *t = checkExpr(*u.operand, expected && expected->isPointer() ? expected->elem
                                                                         : nullptr);
      if (!isLvalue(*u.operand))
        error(u.loc, "cannot take the address of a temporary value");
      return u.type = tc.pointerTo(t);
    }
    case UnaryOp::Deref: {
      Type *t = checkExpr(*u.operand, nullptr);
      if (!t->isPointer())
        error(u.loc, "cannot dereference a value of type " + t->str());
      if (t->elem->isVoid())
        error(u.loc, "cannot dereference *void; cast it to a typed pointer first");
      return u.type = t->elem;
    }
    }
    return nullptr;
  }

  Type *binaryResultType(BinaryOp op, Expr &lhs, Expr &rhs, SourceLoc loc) {
    Type *l = lhs.type, *r = rhs.type;
    auto mismatch = [&]() -> Type * {
      error(loc, std::string("invalid operands to '") + binaryOpStr(op) + "': " +
                     l->str() + " and " + r->str());
    };
    switch (op) {
    case BinaryOp::Add:
    case BinaryOp::Sub:
      if (l->isPointer() && r->isInt()) {
        if (l->elem->isVoid())
          error(loc, "pointer arithmetic on *void");
        return l;
      }
      if (op == BinaryOp::Sub && l->isPointer() && l == r) {
        if (l->elem->isVoid())
          error(loc, "pointer arithmetic on *void");
        return tc.intTy(64, true);
      }
      [[fallthrough]];
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Rem:
      if (l == r && l->isNumeric())
        return l;
      return mismatch();
    case BinaryOp::BitAnd:
    case BinaryOp::BitOr:
    case BinaryOp::BitXor:
    case BinaryOp::Shl:
    case BinaryOp::Shr:
      if (l == r && l->isInt())
        return l;
      return mismatch();
    case BinaryOp::Eq:
    case BinaryOp::Ne:
      if (l == r && l->isScalar())
        return tc.boolTy();
      return mismatch();
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
      if (l == r && (l->isNumeric() || l->isPointer()))
        return tc.boolTy();
      return mismatch();
    case BinaryOp::And:
    case BinaryOp::Or:
      if (l->isBool() && r->isBool())
        return tc.boolTy();
      return mismatch();
    }
    return nullptr;
  }

  static bool castAllowed(Type *from, Type *to) {
    if (from == to)
      return true;
    if (from->isNumeric() && to->isNumeric())
      return true;
    if (from->isBool() && to->isInt())
      return true;
    if (from->isPointer() && to->isPointer())
      return true;
    if ((from->isPointer() && to->isInt()) || (from->isInt() && to->isPointer()))
      return true;
    return false;
  }

  bool isLvalue(const Expr &e) {
    switch (e.kind) {
    case ExprKind::Ident:
      return true;
    case ExprKind::Unary:
      return static_cast<const UnaryExpr &>(e).op == UnaryOp::Deref;
    case ExprKind::Member: {
      auto &me = static_cast<const MemberExpr &>(e);
      return me.throughPointer || isLvalue(*me.base);
    }
    case ExprKind::Index: {
      auto &ix = static_cast<const IndexExpr &>(e);
      return ix.base->type->isPointer() || isLvalue(*ix.base);
    }
    default:
      return false;
    }
  }

  void checkMutable(const Expr &e) {
    switch (e.kind) {
    case ExprKind::Ident: {
      auto &id = static_cast<const IdentExpr &>(e);
      if (!id.var->isMutable)
        error(e.loc, "cannot assign to immutable variable '" + id.name +
                         "' (declare it with 'var')");
      return;
    }
    case ExprKind::Unary:
      if (static_cast<const UnaryExpr &>(e).op == UnaryOp::Deref)
        return;
      break;
    case ExprKind::Member: {
      auto &me = static_cast<const MemberExpr &>(e);
      if (!me.throughPointer)
        checkMutable(*me.base);
      return;
    }
    case ExprKind::Index: {
      auto &ix = static_cast<const IndexExpr &>(e);
      if (!ix.base->type->isPointer())
        checkMutable(*ix.base);
      return;
    }
    default:
      break;
    }
    error(e.loc, "invalid assignment target");
  }

  Type *checkCall(CallExpr &c) {
    auto it = funcs.find(c.callee);
    if (it == funcs.end()) {
      if (lookupVar(c.callee))
        error(c.loc, "'" + c.callee + "' is a variable, not a function");
      error(c.loc, "call to undeclared function '" + c.callee + "'");
    }
    FuncDecl *fn = c.fn = it->second;
    size_t n = fn->params.size();
    if (c.args.size() < n || (!fn->isVariadic && c.args.size() > n))
      error(c.loc, "'" + fn->name + "' expects " + (fn->isVariadic ? "at least " : "") +
                       std::to_string(n) + " argument" + (n == 1 ? "" : "s") + ", got " +
                       std::to_string(c.args.size()));
    for (size_t i = 0; i < c.args.size(); ++i) {
      if (i < n) {
        Type *want = fn->params[i].var.type;
        checkExpr(*c.args[i], want);
        requireType(*c.args[i], want);
      } else {
        Type *t = checkExpr(*c.args[i], nullptr);
        if (!t->isScalar())
          error(c.args[i]->loc, "cannot pass a value of type " + t->str() +
                                    " as a variadic argument");
      }
    }
    return c.type = fn->retType;
  }

  Type *checkStructLit(StructLitExpr &s) {
    auto it = structs.find(s.name);
    if (it == structs.end())
      error(s.loc, "unknown struct '" + s.name + "'");
    StructDecl *decl = it->second;
    std::vector<bool> seen(decl->fields.size());
    for (auto &f : s.fields) {
      int idx = decl->fieldIndex(f.name);
      if (idx < 0)
        error(f.loc, "struct '" + decl->name + "' has no field '" + f.name + "'");
      if (seen[idx])
        error(f.loc, "field '" + f.name + "' is initialized twice");
      seen[idx] = true;
      f.index = idx;
      Type *want = decl->fields[idx].type;
      checkExpr(*f.value, want);
      requireType(*f.value, want);
    }
    return s.type = decl->type;
  }

  Type *checkArrayLit(ArrayLitExpr &a, Type *expected) {
    if (a.elems.empty())
      error(a.loc, "empty array literal");
    Type *elem = expected && expected->isArray() ? expected->elem : nullptr;
    if (expected && expected->isArray() && expected->count != a.elems.size())
      error(a.loc, "expected " + std::to_string(expected->count) + " elements, found " +
                       std::to_string(a.elems.size()));
    for (auto &e : a.elems) {
      checkExpr(*e, elem);
      if (!elem)
        elem = e->type;
      requireType(*e, elem);
    }
    if (elem->isVoid())
      error(a.loc, "array element type cannot be void");
    return a.type = tc.arrayOf(elem, a.elems.size());
  }
};

} // namespace

void analyze(Module &m, TypeContext &types) { Sema(m, types).run(); }

} // namespace hoshi
