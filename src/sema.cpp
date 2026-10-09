#include "sema.h"

#include <cmath>
#include <set>
#include <unordered_map>

namespace hoshi {

namespace {

using i128 = __int128;
using u128 = unsigned __int128;

constexpr int kMaxInstantiationDepth = 64;

bool fitsType(i128 v, Type *t) {
  if (t->isSigned)
    return v >= -(i128(1) << (t->bits - 1)) && v < (i128(1) << (t->bits - 1));
  return v >= 0 && v < (i128(1) << t->bits);
}

// Two's complement wrap-around, as a runtime cast would do.
i128 wrapInt(i128 v, Type *t) {
  u128 mask = (u128(1) << t->bits) - 1;
  u128 u = u128(v) & mask;
  if (t->isSigned && u >= (u128(1) << (t->bits - 1)))
    return i128(u) - (i128(1) << t->bits);
  return i128(u);
}

std::string typeListStr(const std::vector<Type *> &ts) {
  std::string s;
  for (size_t i = 0; i < ts.size(); ++i)
    s += (i ? ", " : "") + ts[i]->str();
  return s;
}

class Sema {
public:
  Sema(Module &m, TypeContext &tc) : m(m), tc(tc) {}

  void run(const std::map<std::string, ConstValue> &predefined,
           const std::map<std::string, ConstValue> &overrides) {
    for (auto &[name, value] : predefined)
      predefinedNames.insert(name);
    std::map<std::string, ConstValue> builtins = predefined;
    for (auto &[name, value] : overrides) {
      builtins[name] = value;
      this->overrides[name] = value;
    }
    for (auto &[name, value] : builtins) {
      auto c = std::make_unique<ConstDecl>();
      c->name = name;
      c->state = ConstDecl::Done;
      c->value = value;
      if (c->value.kind == ConstValue::Str && !c->value.type)
        c->value.type = tc.pointerTo(tc.intTy(8, false));
      builtinConsts[name] = c.get();
      m.builtinConsts.push_back(std::move(c));
    }

    for (auto &c : m.consts)
      registerConst(*c);
    for (auto &s : m.structs)
      registerStruct(*s);
    for (auto &f : m.funcs)
      registerFunc(*f);
    expandWhens(m.whens);
    m.whens.clear();

    for (auto &c : m.consts)
      inGlobalContext(nullptr, [&] { resolveConst(*c); });
    for (auto &s : m.structs)
      if (!s->isGeneric())
        resolveFields(*s);
    for (auto &f : m.funcs)
      if (!f->isGeneric())
        declareFunc(*f);
    for (auto &f : m.funcs)
      if (!f->isGeneric() && f->body)
        checkFuncBody(*f);

    // Instantiating one generic body may request more instances.
    while (!pendingBodies.empty()) {
      FuncDecl *f = pendingBodies.back();
      pendingBodies.pop_back();
      withInstanceNote(*f, [&] { checkFuncBody(*f); });
    }

    for (auto &s : m.structs)
      if (!s->isGeneric())
        checkStructCycle(s.get());
    for (size_t i = 0; i < m.structInstances.size(); ++i)
      checkStructCycle(m.structInstances[i].get());
  }

private:
  Module &m;
  TypeContext &tc;

  struct Symbol {
    VarSym *var = nullptr;
    ConstDecl *constant = nullptr;
  };

  std::unordered_map<std::string, StructDecl *> structs;
  std::unordered_map<std::string, FuncDecl *> funcs;
  std::unordered_map<std::string, ConstDecl *> globalConsts;
  std::unordered_map<std::string, ConstDecl *> builtinConsts;
  std::set<std::string> predefinedNames;            // OS, ARCH, OPT_LEVEL
  std::map<std::string, ConstValue> overrides;      // -D values
  std::map<ConstDecl *, ConstValue> overriddenConsts; // global consts replaced by -D
  std::vector<std::unordered_map<std::string, Symbol>> scopes;
  const TypeEnv *typeEnv = nullptr; // bindings of the generic being checked
  FuncDecl *curFn = nullptr;
  int loopDepth = 0;
  int deferDepth = 0;

  std::map<std::pair<StructDecl *, std::vector<Type *>>, StructDecl *> structCache;
  std::map<std::pair<FuncDecl *, std::vector<Type *>>, FuncDecl *> funcCache;
  std::vector<FuncDecl *> pendingBodies;
  int instantiationDepth = 0;

  // Run `fn` as if at the top level: no local scopes, and the given generic
  // bindings. Used for global constants and generic instantiation, which must
  // not see the locals of whatever function triggered them.
  template <typename Fn> void inGlobalContext(const TypeEnv *env, Fn &&fn) {
    auto savedScopes = std::move(scopes);
    scopes.clear();
    const TypeEnv *savedEnv = typeEnv;
    FuncDecl *savedFn = curFn;
    int savedLoop = loopDepth, savedDefer = deferDepth;
    typeEnv = env;
    struct Restore {
      Sema &s;
      decltype(savedScopes) &sc;
      const TypeEnv *env;
      FuncDecl *fn;
      int loop, defer;
      ~Restore() {
        s.scopes = std::move(sc);
        s.typeEnv = env;
        s.curFn = fn;
        s.loopDepth = loop;
        s.deferDepth = defer;
      }
    } restore{*this, savedScopes, savedEnv, savedFn, savedLoop, savedDefer};
    fn();
  }

  template <typename Fn> void withInstanceNote(FuncDecl &inst, Fn &&fn) {
    try {
      fn();
    } catch (CompileError &e) {
      e.notes.push_back({inst.requestLoc, "in instantiation of '" + inst.name + "' requested here"});
      throw;
    }
  }

  // ---- Registration and `when` expansion -----------------------------------

  void checkTypeParams(const std::vector<TypeParam> &params) {
    for (size_t i = 0; i < params.size(); ++i)
      for (size_t j = 0; j < i; ++j)
        if (params[i].name == params[j].name)
          error(params[i].loc, "duplicate type parameter '" + params[i].name + "'");
  }

  void registerConst(ConstDecl &c) {
    if (funcs.count(c.name))
      error(c.loc, "'" + c.name + "' is already declared as a function");
    if (predefinedNames.count(c.name))
      error(c.loc, "'" + c.name + "' is a predefined constant");
    if (!globalConsts.emplace(c.name, &c).second)
      error(c.loc, "redefinition of constant '" + c.name + "'");
    // `-D NAME=VALUE` replaces the declared value, which then acts as a default.
    if (auto it = overrides.find(c.name); it != overrides.end())
      overriddenConsts[&c] = it->second;
  }

  void registerStruct(StructDecl &s) {
    if (tc.builtin(s.name))
      error(s.loc, "'" + s.name + "' is a builtin type name");
    if (!structs.emplace(s.name, &s).second)
      error(s.loc, "redefinition of struct '" + s.name + "'");
    checkTypeParams(s.typeParams);
    if (!s.isGeneric())
      s.type = tc.structTy(&s);
  }

  void registerFunc(FuncDecl &f) {
    if (globalConsts.count(f.name))
      error(f.loc, "'" + f.name + "' is already declared as a constant");
    if (!funcs.emplace(f.name, &f).second)
      error(f.loc, "redefinition of function '" + f.name + "'");
    checkTypeParams(f.typeParams);
    if (f.isGeneric() && f.name == "main")
      error(f.loc, "'main' cannot be generic");
  }

  // Pick the active branch of each top-level `when` and splice its
  // declarations into the module.
  void expandWhens(std::vector<std::unique_ptr<WhenDecl>> &whens) {
    for (auto &w : whens) {
      bool cond = false;
      inGlobalContext(nullptr, [&] { cond = evalCondition(*w->cond); });
      Module &chosen = cond ? w->thenItems : w->elseItems;
      for (auto &c : chosen.consts) {
        registerConst(*c);
        m.consts.push_back(std::move(c));
      }
      for (auto &s : chosen.structs) {
        registerStruct(*s);
        m.structs.push_back(std::move(s));
      }
      for (auto &f : chosen.funcs) {
        registerFunc(*f);
        m.funcs.push_back(std::move(f));
      }
      expandWhens(chosen.whens);
    }
  }

  bool evalCondition(Expr &e) {
    ConstValue v = evalConst(e);
    if (v.kind != ConstValue::Bool)
      error(e.loc, "'when' condition must be a compile-time bool");
    return v.b;
  }

  // ---- Types --------------------------------------------------------------

  Type *resolveType(TypeRef &t) {
    switch (t.kind) {
    case TypeRef::Named:
      t.resolved = resolveNamedType(t);
      break;
    case TypeRef::Pointer:
      t.resolved = tc.pointerTo(resolveType(*t.elem));
      break;
    case TypeRef::Array: {
      Type *elem = resolveType(*t.elem);
      if (elem->isVoid())
        error(t.loc, "array element type cannot be void");
      ConstValue n = evalConst(*t.count);
      if (n.kind != ConstValue::Int)
        error(t.count->loc, "array length must be a compile-time integer");
      if (n.i <= 0)
        error(t.count->loc, "array length must be greater than zero");
      if (n.i > (i128(1) << 40))
        error(t.count->loc, "array length is too large");
      t.resolved = tc.arrayOf(elem, uint64_t(n.i));
      break;
    }
    }
    return t.resolved;
  }

  Type *resolveNamedType(TypeRef &t) {
    if (typeEnv) {
      if (auto it = typeEnv->find(t.name); it != typeEnv->end()) {
        if (!t.args.empty())
          error(t.loc, "type parameter '" + t.name + "' does not take type arguments");
        return it->second;
      }
    }
    if (Type *b = tc.builtin(t.name)) {
      if (!t.args.empty())
        error(t.loc, "'" + t.name + "' does not take type arguments");
      return b;
    }
    auto it = structs.find(t.name);
    if (it == structs.end())
      error(t.loc, "unknown type '" + t.name + "'");
    StructDecl *s = it->second;
    if (!s->isGeneric()) {
      if (!t.args.empty())
        error(t.loc, "struct '" + s->name + "' is not generic");
      return s->type;
    }
    return instantiateStruct(*s, resolveTypeArgs(t.args, *s, t.loc), t.loc)->type;
  }

  std::vector<Type *> resolveTypeArgs(std::vector<std::unique_ptr<TypeRef>> &args,
                                      StructDecl &s, SourceLoc loc) {
    if (args.empty())
      error(loc, "generic struct '" + s.name + "' needs type arguments, e.g. " + s.name + "[" +
                     s.typeParams[0].name + "]");
    if (args.size() != s.typeParams.size())
      error(loc, "struct '" + s.name + "' takes " + std::to_string(s.typeParams.size()) +
                     " type argument(s), got " + std::to_string(args.size()));
    std::vector<Type *> out;
    for (auto &a : args)
      out.push_back(resolveValueType(*a, "a type argument"));
    return out;
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
      error(s->loc, "struct '" + s->name + "' contains itself by value; use a pointer instead");
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

  // ---- Generics -----------------------------------------------------------

  StructDecl *instantiateStruct(StructDecl &templ, const std::vector<Type *> &args,
                                SourceLoc loc) {
    auto key = std::make_pair(&templ, args);
    if (auto it = structCache.find(key); it != structCache.end())
      return it->second;
    if (instantiationDepth >= kMaxInstantiationDepth)
      error(loc, "generic instantiation of '" + templ.name +
                     "' is nested too deeply (is it infinitely recursive?)");

    auto inst = std::make_unique<StructDecl>();
    inst->name = templ.name + "[" + typeListStr(args) + "]";
    inst->loc = templ.loc;
    inst->templ = &templ;
    inst->typeArgs = args;
    for (auto &f : templ.fields)
      inst->fields.push_back(Field{f.name, f.loc, cloneTypeRef(*f.typeRef)});
    inst->type = tc.structTy(inst.get());
    StructDecl *raw = inst.get();
    m.structInstances.push_back(std::move(inst));
    structCache[key] = raw; // before resolving fields: `next: *Node[T]`

    TypeEnv env;
    for (size_t i = 0; i < args.size(); ++i)
      env[templ.typeParams[i].name] = args[i];
    ++instantiationDepth;
    try {
      inGlobalContext(&env, [&] { resolveFields(*raw); });
    } catch (CompileError &e) {
      e.notes.push_back({loc, "in instantiation of '" + raw->name + "' requested here"});
      throw;
    }
    --instantiationDepth;
    return raw;
  }

  FuncDecl *instantiateFunc(FuncDecl &templ, const std::vector<Type *> &args, SourceLoc loc) {
    auto key = std::make_pair(&templ, args);
    if (auto it = funcCache.find(key); it != funcCache.end())
      return it->second;
    if (instantiationDepth >= kMaxInstantiationDepth)
      error(loc, "generic instantiation of '" + templ.name +
                     "' is nested too deeply (is it infinitely recursive?)");

    auto inst = std::make_unique<FuncDecl>();
    inst->name = templ.name + "[" + typeListStr(args) + "]";
    inst->loc = templ.loc;
    inst->isInstance = true;
    inst->requestLoc = loc;
    for (auto &p : templ.params) {
      Param np;
      np.var.name = p.var.name;
      np.var.loc = p.var.loc;
      np.typeRef = cloneTypeRef(*p.typeRef);
      inst->params.push_back(std::move(np));
    }
    if (templ.retRef)
      inst->retRef = cloneTypeRef(*templ.retRef);
    inst->body.reset(static_cast<BlockStmt *>(cloneStmt(*templ.body).release()));
    for (size_t i = 0; i < args.size(); ++i)
      inst->typeEnv[templ.typeParams[i].name] = args[i];

    FuncDecl *raw = inst.get();
    m.funcInstances.push_back(std::move(inst));
    funcCache[key] = raw;

    ++instantiationDepth;
    withInstanceNote(*raw, [&] { inGlobalContext(&raw->typeEnv, [&] { declareFunc(*raw); }); });
    --instantiationDepth;
    pendingBodies.push_back(raw);
    return raw;
  }

  // Bind type parameters by matching a parameter's declared type against the
  // type of the argument passed for it.
  void unify(TypeRef &pat, Type *actual, const FuncDecl &fn, std::vector<Type *> &bound,
             SourceLoc loc) {
    switch (pat.kind) {
    case TypeRef::Named: {
      for (size_t i = 0; i < fn.typeParams.size(); ++i) {
        if (fn.typeParams[i].name != pat.name)
          continue;
        if (!bound[i])
          bound[i] = actual;
        else if (bound[i] != actual)
          error(loc, "conflicting types for type parameter '" + pat.name + "': " +
                         bound[i]->str() + " and " + actual->str());
        return;
      }
      if (!pat.args.empty() && actual->isStruct() && actual->decl->templ &&
          actual->decl->templ->name == pat.name &&
          actual->decl->typeArgs.size() == pat.args.size())
        for (size_t i = 0; i < pat.args.size(); ++i)
          unify(*pat.args[i], actual->decl->typeArgs[i], fn, bound, loc);
      return;
    }
    case TypeRef::Pointer:
      if (actual->isPointer())
        unify(*pat.elem, actual->elem, fn, bound, loc);
      return;
    case TypeRef::Array:
      if (actual->isArray())
        unify(*pat.elem, actual->elem, fn, bound, loc);
      return;
    }
  }

  // The parameter type with the bindings so far, or null if some type
  // parameter it mentions is still unknown.
  Type *paramTypeSoFar(const TypeRef &pat, const FuncDecl &fn,
                       const std::vector<Type *> &bound) {
    TypeEnv env;
    for (size_t i = 0; i < bound.size(); ++i) {
      if (bound[i])
        env[fn.typeParams[i].name] = bound[i];
      else if (typeParamUsed(pat, fn.typeParams[i].name))
        return nullptr;
    }
    auto copy = cloneTypeRef(pat);
    Type *t = nullptr;
    inGlobalContext(&env, [&] { t = resolveType(*copy); });
    return t;
  }

  static bool typeParamUsed(const TypeRef &t, const std::string &name) {
    if (t.kind == TypeRef::Named) {
      if (t.name == name)
        return true;
      for (auto &a : t.args)
        if (typeParamUsed(*a, name))
          return true;
      return false;
    }
    return typeParamUsed(*t.elem, name);
  }

  // ---- Constants ----------------------------------------------------------

  void resolveConst(ConstDecl &c) {
    if (c.state == ConstDecl::Done)
      return;
    if (c.state == ConstDecl::Resolving)
      error(c.loc, "constant '" + c.name + "' depends on itself");
    c.state = ConstDecl::Resolving;
    Type *declared = c.typeRef ? resolveValueType(*c.typeRef, "a constant") : nullptr;
    ConstValue v = evalConst(*c.init);
    if (auto ov = overriddenConsts.find(&c); ov != overriddenConsts.end()) {
      bool numeric = (v.kind == ConstValue::Int || v.kind == ConstValue::Float) &&
                     (ov->second.kind == ConstValue::Int || ov->second.kind == ConstValue::Float);
      if (v.kind != ov->second.kind && !numeric)
        error(c.loc, "-D " + c.name + "=" + ov->second.str() + " does not match the type of '" +
                         c.name + "' (" + constTypeName(v) + ")");
      v = ov->second;
    }
    c.value = declared ? convertConst(v, declared, c.init->loc) : v;
    c.state = ConstDecl::Done;
  }

  ConstValue convertConst(ConstValue v, Type *to, SourceLoc loc) {
    auto mismatch = [&]() -> ConstValue {
      error(loc, "expected " + to->str() + ", found " + constTypeName(v));
    };
    if (v.type && v.type != to)
      return mismatch();
    switch (v.kind) {
    case ConstValue::Int:
      if (to->isInt()) {
        if (!fitsType(v.i, to))
          error(loc, "constant " + v.str() + " does not fit in " + to->str());
        return ConstValue::ofInt(v.i, to);
      }
      if (to->isFloat())
        return ConstValue::ofFloat(double(v.i), to);
      return mismatch();
    case ConstValue::Float:
      if (to->isFloat())
        return ConstValue::ofFloat(to->bits == 32 ? double(float(v.f)) : v.f, to);
      return mismatch();
    case ConstValue::Bool:
      if (to->isBool()) {
        v.type = to;
        return v;
      }
      return mismatch();
    case ConstValue::Str:
      if (to == tc.pointerTo(tc.intTy(8, false))) {
        v.type = to;
        return v;
      }
      return mismatch();
    }
    return v;
  }

  std::string constTypeName(const ConstValue &v) {
    if (v.type)
      return v.type->str();
    switch (v.kind) {
    case ConstValue::Int: return "integer constant";
    case ConstValue::Float: return "float constant";
    case ConstValue::Bool: return "bool";
    case ConstValue::Str: return "*u8";
    }
    return "?";
  }

  ConstValue checkedInt(i128 v, Type *t, SourceLoc loc) {
    bool ok = t ? fitsType(v, t) : (v >= -(i128(1) << 64) && v < (i128(1) << 64));
    if (!ok)
      error(loc, "constant overflow" + (t ? " in " + t->str() : std::string()));
    return ConstValue::ofInt(v, t);
  }

  ConstValue evalConst(Expr &e) {
    switch (e.kind) {
    case ExprKind::IntLit:
      return ConstValue::ofInt(i128(static_cast<IntLitExpr &>(e).value));
    case ExprKind::FloatLit:
      return ConstValue::ofFloat(static_cast<FloatLitExpr &>(e).value);
    case ExprKind::BoolLit:
      return ConstValue::ofBool(static_cast<BoolLitExpr &>(e).value);
    case ExprKind::StringLit:
      return ConstValue::ofStr(static_cast<StringLitExpr &>(e).value);
    case ExprKind::Ident: {
      auto &id = static_cast<IdentExpr &>(e);
      Symbol sym = lookup(id.name);
      if (sym.var)
        error(e.loc, "'" + id.name + "' is a variable, not a compile-time constant");
      if (!sym.constant)
        error(e.loc, "use of undeclared constant '" + id.name + "'");
      return constValueOf(*sym.constant);
    }
    case ExprKind::Unary: {
      auto &u = static_cast<UnaryExpr &>(e);
      ConstValue v = evalConst(*u.operand);
      switch (u.op) {
      case UnaryOp::Neg:
        if (v.kind == ConstValue::Int) {
          if (v.type && !v.type->isSigned)
            error(u.loc, "cannot negate a value of type " + v.type->str());
          return checkedInt(-v.i, v.type, u.loc);
        }
        if (v.kind == ConstValue::Float)
          return ConstValue::ofFloat(-v.f, v.type);
        break;
      case UnaryOp::Not:
        if (v.kind == ConstValue::Bool)
          return ConstValue::ofBool(!v.b);
        break;
      case UnaryOp::BitNot:
        if (v.kind == ConstValue::Int)
          return ConstValue::ofInt(v.type ? wrapInt(~v.i, v.type) : ~v.i, v.type);
        break;
      default:
        break;
      }
      error(u.loc, std::string("invalid operand to '") + unaryOpStr(u.op) +
                       "' in a constant expression");
    }
    case ExprKind::Binary:
      return evalConstBinary(static_cast<BinaryExpr &>(e));
    case ExprKind::Cast: {
      auto &c = static_cast<CastExpr &>(e);
      ConstValue v = evalConst(*c.operand);
      Type *to = resolveType(*c.target);
      if (to->isInt()) {
        if (v.kind == ConstValue::Int)
          return ConstValue::ofInt(wrapInt(v.i, to), to);
        if (v.kind == ConstValue::Bool)
          return ConstValue::ofInt(v.b ? 1 : 0, to);
        if (v.kind == ConstValue::Float) {
          if (!std::isfinite(v.f) || std::fabs(v.f) >= 1.7e38)
            error(c.loc, "float constant is out of range for " + to->str());
          return ConstValue::ofInt(wrapInt(i128(v.f), to), to);
        }
      }
      if (to->isFloat()) {
        double f = v.kind == ConstValue::Int     ? double(v.i)
                   : v.kind == ConstValue::Float ? v.f
                                                 : NAN;
        if (v.kind == ConstValue::Int || v.kind == ConstValue::Float)
          return ConstValue::ofFloat(to->bits == 32 ? double(float(f)) : f, to);
      }
      if (to->isBool() && v.kind == ConstValue::Bool)
        return v;
      error(c.loc, "cannot evaluate this cast at compile time");
    }
    case ExprKind::SizeOf:
      error(e.loc, "sizeof is not supported in constant expressions yet");
    default:
      error(e.loc, "expression is not a compile-time constant");
    }
  }

  ConstValue evalConstBinary(BinaryExpr &b) {
    ConstValue l = evalConst(*b.lhs);
    if (b.op == BinaryOp::And || b.op == BinaryOp::Or) {
      // Short-circuit like the runtime operator.
      if (l.kind != ConstValue::Bool)
        error(b.lhs->loc, "operand of '" + std::string(binaryOpStr(b.op)) + "' must be bool");
      if (b.op == BinaryOp::And ? !l.b : l.b)
        return l;
      ConstValue r = evalConst(*b.rhs);
      if (r.kind != ConstValue::Bool)
        error(b.rhs->loc, "operand of '" + std::string(binaryOpStr(b.op)) + "' must be bool");
      return r;
    }
    ConstValue r = evalConst(*b.rhs);
    auto invalid = [&]() -> ConstValue {
      error(b.loc, std::string("invalid operands to '") + binaryOpStr(b.op) + "': " +
                       constTypeName(l) + " and " + constTypeName(r));
    };
    bool isCmp = b.op == BinaryOp::Eq || b.op == BinaryOp::Ne || b.op == BinaryOp::Lt ||
                 b.op == BinaryOp::Le || b.op == BinaryOp::Gt || b.op == BinaryOp::Ge;

    if (l.kind == ConstValue::Str || r.kind == ConstValue::Str ||
        l.kind == ConstValue::Bool || r.kind == ConstValue::Bool) {
      if (l.kind != r.kind || (b.op != BinaryOp::Eq && b.op != BinaryOp::Ne))
        return invalid();
      bool eq = l.kind == ConstValue::Str ? l.s == r.s : l.b == r.b;
      return ConstValue::ofBool(b.op == BinaryOp::Eq ? eq : !eq);
    }

    if (l.type && r.type && l.type != r.type)
      return invalid();
    Type *t = l.type ? l.type : r.type;
    bool isFloat = l.kind == ConstValue::Float || r.kind == ConstValue::Float ||
                   (t && t->isFloat());
    if (t && t->isFloat() != isFloat)
      return invalid(); // e.g. i32 constant with a float literal

    if (isFloat) {
      double x = l.kind == ConstValue::Float ? l.f : double(l.i);
      double y = r.kind == ConstValue::Float ? r.f : double(r.i);
      switch (b.op) {
      case BinaryOp::Add: return ConstValue::ofFloat(x + y, t);
      case BinaryOp::Sub: return ConstValue::ofFloat(x - y, t);
      case BinaryOp::Mul: return ConstValue::ofFloat(x * y, t);
      case BinaryOp::Div: return ConstValue::ofFloat(x / y, t);
      case BinaryOp::Rem: return ConstValue::ofFloat(std::fmod(x, y), t);
      case BinaryOp::Eq: return ConstValue::ofBool(x == y);
      case BinaryOp::Ne: return ConstValue::ofBool(x != y);
      case BinaryOp::Lt: return ConstValue::ofBool(x < y);
      case BinaryOp::Le: return ConstValue::ofBool(x <= y);
      case BinaryOp::Gt: return ConstValue::ofBool(x > y);
      case BinaryOp::Ge: return ConstValue::ofBool(x >= y);
      default: return invalid();
      }
    }

    i128 x = l.i, y = r.i;
    if (isCmp) {
      switch (b.op) {
      case BinaryOp::Eq: return ConstValue::ofBool(x == y);
      case BinaryOp::Ne: return ConstValue::ofBool(x != y);
      case BinaryOp::Lt: return ConstValue::ofBool(x < y);
      case BinaryOp::Le: return ConstValue::ofBool(x <= y);
      case BinaryOp::Gt: return ConstValue::ofBool(x > y);
      default: return ConstValue::ofBool(x >= y);
      }
    }
    switch (b.op) {
    case BinaryOp::Add: return checkedInt(x + y, t, b.loc);
    case BinaryOp::Sub: return checkedInt(x - y, t, b.loc);
    case BinaryOp::Mul: {
      i128 p;
      if (__builtin_mul_overflow(x, y, &p))
        error(b.loc, "constant overflow");
      return checkedInt(p, t, b.loc);
    }
    case BinaryOp::Div:
    case BinaryOp::Rem:
      if (y == 0)
        error(b.loc, "division by zero in a constant expression");
      return checkedInt(b.op == BinaryOp::Div ? x / y : x % y, t, b.loc);
    case BinaryOp::BitAnd: return ConstValue::ofInt(t ? wrapInt(x & y, t) : x & y, t);
    case BinaryOp::BitOr: return ConstValue::ofInt(t ? wrapInt(x | y, t) : x | y, t);
    case BinaryOp::BitXor: return ConstValue::ofInt(t ? wrapInt(x ^ y, t) : x ^ y, t);
    case BinaryOp::Shl:
    case BinaryOp::Shr: {
      int width = t ? int(t->bits) : 64;
      if (y < 0 || y >= width)
        error(b.rhs->loc, "shift amount " + r.str() + " is out of range");
      if (b.op == BinaryOp::Shr)
        return ConstValue::ofInt(x >> int(y), t);
      i128 p;
      if (__builtin_mul_overflow(x, i128(1) << int(y), &p))
        error(b.loc, "constant overflow");
      return checkedInt(p, t, b.loc);
    }
    default:
      return invalid();
    }
  }

  ConstValue constValueOf(ConstDecl &c) {
    if (c.state != ConstDecl::Done) {
      // A global constant referenced before its declaration was checked.
      inGlobalContext(nullptr, [&] { resolveConst(c); });
    }
    return c.value;
  }

  // ---- Functions ----------------------------------------------------------

  void declareFunc(FuncDecl &f) {
    const TypeEnv *savedEnv = typeEnv;
    typeEnv = f.isInstance ? &f.typeEnv : nullptr;
    for (auto &p : f.params) {
      p.var.type = resolveValueType(*p.typeRef, "a parameter");
      p.var.isMutable = false;
    }
    f.retType = f.retRef ? resolveType(*f.retRef) : tc.voidTy();
    typeEnv = savedEnv;

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
    inGlobalContext(f.isInstance ? &f.typeEnv : nullptr, [&] {
      curFn = &f;
      loopDepth = deferDepth = 0;
      scopes.emplace_back();
      for (auto &p : f.params) {
        if (scopes.back().count(p.var.name))
          error(p.var.loc, "duplicate parameter '" + p.var.name + "'");
        scopes.back()[p.var.name].var = &p.var;
      }
      checkBlock(*f.body);

      bool implicitReturnOk = f.retType->isVoid() || f.name == "main";
      if (!implicitReturnOk && !alwaysReturns(*f.body))
        error(f.loc, "function '" + f.name + "' does not return a value on every path");
    });
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
    case StmtKind::When: {
      auto &w = static_cast<const WhenStmt &>(s);
      return w.chosen && containsBreak(*w.chosen);
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
    case StmtKind::When: {
      auto &w = static_cast<const WhenStmt &>(s);
      return w.chosen && alwaysReturns(*w.chosen);
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

  void declareVar(VarSym &v) { scopes.back()[v.name] = Symbol{&v, nullptr}; }

  Symbol lookup(const std::string &name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
      if (auto f = it->find(name); f != it->end())
        return f->second;
    if (auto it = globalConsts.find(name); it != globalConsts.end())
      return Symbol{nullptr, it->second};
    if (auto it = builtinConsts.find(name); it != builtinConsts.end())
      return Symbol{nullptr, it->second};
    return {};
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
    case StmtKind::Const: {
      ConstDecl &c = *static_cast<ConstStmt &>(s).decl;
      resolveConst(c);
      scopes.back()[c.name] = Symbol{nullptr, &c};
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
    case StmtKind::When: {
      auto &w = static_cast<WhenStmt &>(s);
      if (evalCondition(*w.cond))
        w.chosen = w.thenBlock.get();
      else
        w.chosen = w.elseStmt.get();
      if (w.chosen)
        checkStmt(*w.chosen);
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
        error(f.end->loc, "range bounds have different types: " + f.start->type->str() +
                              " and " + f.end->type->str());
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

  // Literals (and untyped constants) whose type is decided by context.
  bool isUntyped(const Expr &e) {
    switch (e.kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::NullLit:
      return true;
    case ExprKind::Ident: {
      Symbol sym = lookup(static_cast<const IdentExpr &>(e).name);
      if (!sym.constant)
        return false;
      ConstValue v = constValueOf(*sym.constant);
      return !v.type && (v.kind == ConstValue::Int || v.kind == ConstValue::Float);
    }
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
      checkExpr(lhs, rhs.type->isPointer() && lhs.kind != ExprKind::NullLit ? nullptr
                                                                             : rhs.type);
    } else {
      checkExpr(lhs, expected);
      checkExpr(rhs, lhs.type->isPointer() && rhs.kind != ExprKind::NullLit ? nullptr
                                                                             : lhs.type);
    }
  }

  static bool fitsInt(uint64_t v, Type *t, bool negated) {
    return fitsType(negated ? -i128(v) : i128(v), t);
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

  // The type a constant takes at a use site.
  Type *constUseType(const ConstValue &v, Type *expected, const IdentExpr &e) {
    if (v.type)
      return v.type;
    switch (v.kind) {
    case ConstValue::Int: {
      Type *t = expected && expected->isNumeric() ? expected : nullptr;
      if (!t) {
        for (Type *cand : {tc.intTy(32, true), tc.intTy(64, true), tc.intTy(64, false)})
          if (fitsType(v.i, cand)) {
            t = cand;
            break;
          }
        if (!t)
          error(e.loc, "constant '" + e.name + "' does not fit in any integer type");
      }
      if (t->isInt() && !fitsType(v.i, t))
        error(e.loc, "constant '" + e.name + "' (" + v.str() + ") does not fit in " + t->str());
      return t;
    }
    case ConstValue::Float:
      return expected && expected->isFloat() ? expected : tc.floatTy(64);
    case ConstValue::Bool:
      return tc.boolTy();
    case ConstValue::Str:
      return tc.pointerTo(tc.intTy(8, false));
    }
    return nullptr;
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
      return e.type = expected && expected->isPointer() ? expected : tc.pointerTo(tc.voidTy());
    case ExprKind::StringLit:
      return e.type = tc.pointerTo(tc.intTy(8, false));
    case ExprKind::Ident: {
      auto &id = static_cast<IdentExpr &>(e);
      Symbol sym = lookup(id.name);
      if (sym.var) {
        id.var = sym.var;
        return e.type = sym.var->type;
      }
      if (sym.constant) {
        id.constDecl = sym.constant;
        return e.type = constUseType(constValueOf(*sym.constant), expected, id);
      }
      if (funcs.count(id.name))
        error(e.loc, "function '" + id.name + "' cannot be used as a value");
      error(e.loc, "use of undeclared variable '" + id.name + "'");
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
      Type *t = checkExpr(*u.operand,
                          expected && expected->isPointer() ? expected->elem : nullptr);
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
      error(loc, std::string("invalid operands to '") + binaryOpStr(op) + "': " + l->str() +
                     " and " + r->str());
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
      return static_cast<const IdentExpr &>(e).var != nullptr;
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
      if (id.constDecl)
        error(e.loc, "cannot assign to constant '" + id.name + "'");
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
      if (lookup(c.callee).var)
        error(c.loc, "'" + c.callee + "' is a variable, not a function");
      error(c.loc, "call to undeclared function '" + c.callee + "'");
    }
    FuncDecl *fn = it->second;
    size_t n = fn->params.size();
    if (c.args.size() < n || (!fn->isVariadic && c.args.size() > n))
      error(c.loc, "'" + fn->name + "' expects " + (fn->isVariadic ? "at least " : "") +
                       std::to_string(n) + " argument" + (n == 1 ? "" : "s") + ", got " +
                       std::to_string(c.args.size()));

    std::vector<bool> checked(c.args.size());
    if (fn->isGeneric()) {
      fn = instantiateForCall(*fn, c, checked);
    } else if (!c.typeArgs.empty()) {
      error(c.loc, "function '" + fn->name + "' is not generic");
    }
    c.fn = fn;

    for (size_t i = 0; i < c.args.size(); ++i) {
      if (i < n) {
        Type *want = fn->params[i].var.type;
        if (!checked[i])
          checkExpr(*c.args[i], want);
        requireType(*c.args[i], want);
      } else {
        Type *t = checkExpr(*c.args[i], nullptr);
        if (!t->isScalar())
          error(c.args[i]->loc,
                "cannot pass a value of type " + t->str() + " as a variadic argument");
      }
    }
    return c.type = fn->retType;
  }

  // Determine the type arguments of a generic call (explicit or inferred
  // from the arguments) and return the matching instance. Arguments checked
  // during inference are marked in `checked`.
  FuncDecl *instantiateForCall(FuncDecl &templ, CallExpr &c, std::vector<bool> &checked) {
    std::vector<Type *> bound(templ.typeParams.size());
    if (!c.typeArgs.empty()) {
      if (c.typeArgs.size() != bound.size())
        error(c.loc, "'" + templ.name + "' takes " + std::to_string(bound.size()) +
                         " type argument(s), got " + std::to_string(c.typeArgs.size()));
      for (size_t i = 0; i < bound.size(); ++i)
        bound[i] = resolveValueType(*c.typeArgs[i], "a type argument");
      return instantiateFunc(templ, bound, c.loc);
    }

    // Typed arguments first, so literals can then adopt the inferred types:
    // in `max(x, 1)` with x: i64, T = i64 and the 1 becomes an i64.
    for (int pass = 0; pass < 2; ++pass) {
      for (size_t i = 0; i < templ.params.size(); ++i) {
        if (checked[i] || isUntyped(*c.args[i]) != (pass == 1))
          continue;
        TypeRef &pat = *templ.params[i].typeRef;
        checkExpr(*c.args[i], paramTypeSoFar(pat, templ, bound));
        checked[i] = true;
        unify(pat, c.args[i]->type, templ, bound, c.args[i]->loc);
      }
    }
    for (size_t i = 0; i < bound.size(); ++i)
      if (!bound[i])
        error(c.loc, "cannot infer type parameter '" + templ.typeParams[i].name + "' of '" +
                         templ.name + "'; specify it explicitly, e.g. " + templ.name + "[" +
                         "i32](...)");
    return instantiateFunc(templ, bound, c.loc);
  }

  Type *checkStructLit(StructLitExpr &s) {
    auto it = structs.find(s.name);
    if (it == structs.end())
      error(s.loc, "unknown struct '" + s.name + "'");
    StructDecl *decl = it->second;
    if (decl->isGeneric())
      decl = instantiateStruct(*decl, resolveTypeArgs(s.typeArgs, *decl, s.loc), s.loc);
    else if (!s.typeArgs.empty())
      error(s.loc, "struct '" + decl->name + "' is not generic");

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

void analyze(Module &m, TypeContext &types, const std::map<std::string, ConstValue> &predefined,
             const std::map<std::string, ConstValue> &overrides) {
  Sema(m, types).run(predefined, overrides);
}

} // namespace hoshi
