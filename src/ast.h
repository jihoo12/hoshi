#pragma once

#include "diag.h"
#include "types.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class Value;
class Function;
} // namespace llvm

namespace hoshi {

struct FuncDecl;
struct ConstDecl;
struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// ---------------------------------------------------------------------------
// Compile-time values

struct ConstValue {
  enum Kind { Int, Float, Bool, Str } kind = Int;
  __int128 i = 0;
  double f = 0;
  bool b = false;
  std::string s;
  Type *type = nullptr; // null: untyped, adopts its type from context

  static ConstValue ofInt(__int128 v, Type *t = nullptr) {
    ConstValue c;
    c.kind = Int;
    c.i = v;
    c.type = t;
    return c;
  }
  static ConstValue ofFloat(double v, Type *t = nullptr) {
    ConstValue c;
    c.kind = Float;
    c.f = v;
    c.type = t;
    return c;
  }
  static ConstValue ofBool(bool v) {
    ConstValue c;
    c.kind = Bool;
    c.b = v;
    return c;
  }
  static ConstValue ofStr(std::string v) {
    ConstValue c;
    c.kind = Str;
    c.s = std::move(v);
    return c;
  }
  std::string str() const;
};

// Names bound to types inside a generic instantiation, e.g. T -> i32.
using TypeEnv = std::map<std::string, Type *>;

// ---------------------------------------------------------------------------
// Type syntax (resolved to Type* by sema)

struct TypeRef {
  enum Kind { Named, Pointer, Array } kind;
  SourceLoc loc;
  std::string name;                           // Named
  std::vector<std::unique_ptr<TypeRef>> args; // Named: generic arguments
  std::unique_ptr<TypeRef> elem;              // Pointer, Array
  ExprPtr count;                              // Array: constant expression
  Type *resolved = nullptr;
};

struct TypeParam {
  std::string name;
  SourceLoc loc;
};

// ---------------------------------------------------------------------------
// Variables

struct VarSym {
  std::string name;
  SourceLoc loc;
  bool isMutable = false;
  Type *type = nullptr;
  llvm::Value *addr = nullptr; // set by codegen (an alloca)
};

// ---------------------------------------------------------------------------
// Expressions

enum class ExprKind {
  IntLit, FloatLit, BoolLit, NullLit, StringLit,
  Ident, Unary, Binary, Call, Member, Index, Cast, SizeOf, StructLit, ArrayLit,
};

enum class UnaryOp { Neg, Not, BitNot, AddrOf, Deref };

enum class BinaryOp {
  Add, Sub, Mul, Div, Rem,
  BitAnd, BitOr, BitXor, Shl, Shr,
  Eq, Ne, Lt, Le, Gt, Ge,
  And, Or,
};

const char *binaryOpStr(BinaryOp op);
const char *unaryOpStr(UnaryOp op);

struct Expr {
  ExprKind kind;
  SourceLoc loc;
  Type *type = nullptr; // set by sema
  Expr(ExprKind kind, SourceLoc loc) : kind(kind), loc(loc) {}
  virtual ~Expr() = default;
};

struct IntLitExpr : Expr {
  uint64_t value;
  bool isChar;
  IntLitExpr(SourceLoc loc, uint64_t value, bool isChar)
      : Expr(ExprKind::IntLit, loc), value(value), isChar(isChar) {}
};

struct FloatLitExpr : Expr {
  double value;
  FloatLitExpr(SourceLoc loc, double value)
      : Expr(ExprKind::FloatLit, loc), value(value) {}
};

struct BoolLitExpr : Expr {
  bool value;
  BoolLitExpr(SourceLoc loc, bool value)
      : Expr(ExprKind::BoolLit, loc), value(value) {}
};

struct NullLitExpr : Expr {
  explicit NullLitExpr(SourceLoc loc) : Expr(ExprKind::NullLit, loc) {}
};

struct StringLitExpr : Expr {
  std::string value;
  StringLitExpr(SourceLoc loc, std::string value)
      : Expr(ExprKind::StringLit, loc), value(std::move(value)) {}
};

struct IdentExpr : Expr {
  std::string name;
  VarSym *var = nullptr;       // set by sema for variables
  ConstDecl *constDecl = nullptr; // set by sema for constants
  IdentExpr(SourceLoc loc, std::string name)
      : Expr(ExprKind::Ident, loc), name(std::move(name)) {}
};

struct UnaryExpr : Expr {
  UnaryOp op;
  ExprPtr operand;
  UnaryExpr(SourceLoc loc, UnaryOp op, ExprPtr operand)
      : Expr(ExprKind::Unary, loc), op(op), operand(std::move(operand)) {}
};

struct BinaryExpr : Expr {
  BinaryOp op;
  ExprPtr lhs, rhs;
  BinaryExpr(SourceLoc loc, BinaryOp op, ExprPtr lhs, ExprPtr rhs)
      : Expr(ExprKind::Binary, loc), op(op), lhs(std::move(lhs)),
        rhs(std::move(rhs)) {}
};

struct CallExpr : Expr {
  std::string callee;
  std::vector<std::unique_ptr<TypeRef>> typeArgs; // explicit: f[i32](...)
  std::vector<ExprPtr> args;
  FuncDecl *fn = nullptr; // set by sema (the instance for generic calls)
  CallExpr(SourceLoc loc, std::string callee, std::vector<ExprPtr> args)
      : Expr(ExprKind::Call, loc), callee(std::move(callee)),
        args(std::move(args)) {}
};

struct MemberExpr : Expr {
  ExprPtr base;
  std::string field;
  unsigned fieldIndex = 0;     // set by sema
  bool throughPointer = false; // set by sema: base is *Struct
  MemberExpr(SourceLoc loc, ExprPtr base, std::string field)
      : Expr(ExprKind::Member, loc), base(std::move(base)),
        field(std::move(field)) {}
};

struct IndexExpr : Expr {
  ExprPtr base, index;
  IndexExpr(SourceLoc loc, ExprPtr base, ExprPtr index)
      : Expr(ExprKind::Index, loc), base(std::move(base)),
        index(std::move(index)) {}
};

struct CastExpr : Expr {
  ExprPtr operand;
  std::unique_ptr<TypeRef> target;
  CastExpr(SourceLoc loc, ExprPtr operand, std::unique_ptr<TypeRef> target)
      : Expr(ExprKind::Cast, loc), operand(std::move(operand)),
        target(std::move(target)) {}
};

struct SizeOfExpr : Expr {
  std::unique_ptr<TypeRef> target;
  SizeOfExpr(SourceLoc loc, std::unique_ptr<TypeRef> target)
      : Expr(ExprKind::SizeOf, loc), target(std::move(target)) {}
};

struct FieldInit {
  std::string name;
  SourceLoc loc;
  ExprPtr value;
  unsigned index = 0; // set by sema
};

struct StructLitExpr : Expr {
  std::string name;
  std::vector<std::unique_ptr<TypeRef>> typeArgs; // Pair[i32] { ... }
  std::vector<FieldInit> fields;
  StructLitExpr(SourceLoc loc, std::string name, std::vector<FieldInit> fields)
      : Expr(ExprKind::StructLit, loc), name(std::move(name)),
        fields(std::move(fields)) {}
};

struct ArrayLitExpr : Expr {
  std::vector<ExprPtr> elems;
  ArrayLitExpr(SourceLoc loc, std::vector<ExprPtr> elems)
      : Expr(ExprKind::ArrayLit, loc), elems(std::move(elems)) {}
};

// ---------------------------------------------------------------------------
// Constants

struct ConstDecl {
  std::string name;
  SourceLoc loc;
  std::unique_ptr<TypeRef> typeRef; // optional
  ExprPtr init;                     // null for predefined constants
  // Set by sema.
  enum State { Unresolved, Resolving, Done } state = Unresolved;
  ConstValue value;
};

// ---------------------------------------------------------------------------
// Statements

enum class StmtKind {
  Block, Let, Const, Expr, Assign, If, When, While, For, Return, Break,
  Continue, Defer,
};

struct Stmt {
  StmtKind kind;
  SourceLoc loc;
  Stmt(StmtKind kind, SourceLoc loc) : kind(kind), loc(loc) {}
  virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct BlockStmt : Stmt {
  std::vector<StmtPtr> stmts;
  explicit BlockStmt(SourceLoc loc) : Stmt(StmtKind::Block, loc) {}
};

struct LetStmt : Stmt {
  VarSym var;
  std::unique_ptr<TypeRef> typeRef; // optional
  ExprPtr init;                     // optional for `var`
  LetStmt(SourceLoc loc) : Stmt(StmtKind::Let, loc) {}
};

struct ConstStmt : Stmt {
  std::unique_ptr<ConstDecl> decl;
  ConstStmt(SourceLoc loc, std::unique_ptr<ConstDecl> decl)
      : Stmt(StmtKind::Const, loc), decl(std::move(decl)) {}
};

struct ExprStmt : Stmt {
  ExprPtr expr;
  ExprStmt(SourceLoc loc, ExprPtr expr)
      : Stmt(StmtKind::Expr, loc), expr(std::move(expr)) {}
};

// `target = value`, or `target op= value` when `compound` is set.
struct AssignStmt : Stmt {
  bool compound = false;
  BinaryOp op = BinaryOp::Add;
  ExprPtr target, value;
  AssignStmt(SourceLoc loc) : Stmt(StmtKind::Assign, loc) {}
};

struct IfStmt : Stmt {
  ExprPtr cond;
  std::unique_ptr<BlockStmt> thenBlock;
  StmtPtr elseStmt; // BlockStmt or IfStmt, optional
  IfStmt(SourceLoc loc) : Stmt(StmtKind::If, loc) {}
};

// Compile-time `if`: only the chosen branch is type checked and compiled.
struct WhenStmt : Stmt {
  ExprPtr cond;
  std::unique_ptr<BlockStmt> thenBlock;
  StmtPtr elseStmt;        // BlockStmt or WhenStmt, optional
  Stmt *chosen = nullptr;  // set by sema; null if no branch applies
  WhenStmt(SourceLoc loc) : Stmt(StmtKind::When, loc) {}
};

struct WhileStmt : Stmt {
  ExprPtr cond;
  std::unique_ptr<BlockStmt> body;
  WhileStmt(SourceLoc loc) : Stmt(StmtKind::While, loc) {}
};

// for i in start..end { body }   (end exclusive)
struct ForStmt : Stmt {
  VarSym var;
  ExprPtr start, end;
  std::unique_ptr<BlockStmt> body;
  ForStmt(SourceLoc loc) : Stmt(StmtKind::For, loc) {}
};

struct ReturnStmt : Stmt {
  ExprPtr value; // optional
  ReturnStmt(SourceLoc loc, ExprPtr value)
      : Stmt(StmtKind::Return, loc), value(std::move(value)) {}
};

struct BreakStmt : Stmt {
  explicit BreakStmt(SourceLoc loc) : Stmt(StmtKind::Break, loc) {}
};

struct ContinueStmt : Stmt {
  explicit ContinueStmt(SourceLoc loc) : Stmt(StmtKind::Continue, loc) {}
};

struct DeferStmt : Stmt {
  StmtPtr body;
  DeferStmt(SourceLoc loc, StmtPtr body)
      : Stmt(StmtKind::Defer, loc), body(std::move(body)) {}
};

// ---------------------------------------------------------------------------
// Declarations

struct Param {
  VarSym var;
  std::unique_ptr<TypeRef> typeRef;
};

struct FuncDecl {
  std::string name;
  SourceLoc loc;
  std::vector<TypeParam> typeParams; // non-empty: a generic template
  std::vector<Param> params;
  std::unique_ptr<TypeRef> retRef; // null means void
  bool isExtern = false;
  bool isVariadic = false;
  std::unique_ptr<BlockStmt> body; // null for extern
  // Set by sema.
  Type *retType = nullptr;
  TypeEnv typeEnv;              // generic instances: T -> concrete type
  bool isInstance = false;      // a monomorphized copy of a generic template
  SourceLoc requestLoc;         // instances: where it was first requested
  // Set by codegen.
  llvm::Function *llvmFn = nullptr;

  bool isGeneric() const { return !typeParams.empty(); }
};

struct Field {
  std::string name;
  SourceLoc loc;
  std::unique_ptr<TypeRef> typeRef;
  Type *type = nullptr; // set by sema
};

struct StructDecl {
  std::string name;
  SourceLoc loc;
  std::vector<TypeParam> typeParams; // non-empty: a generic template
  std::vector<Field> fields;
  // Set by sema.
  Type *type = nullptr;
  StructDecl *templ = nullptr;   // instances: the generic template
  std::vector<Type *> typeArgs;  // instances: the arguments, in order

  bool isGeneric() const { return !typeParams.empty(); }

  int fieldIndex(const std::string &n) const {
    for (size_t i = 0; i < fields.size(); ++i)
      if (fields[i].name == n)
        return (int)i;
    return -1;
  }
};

struct WhenDecl;

struct Module {
  std::vector<std::unique_ptr<StructDecl>> structs;
  std::vector<std::unique_ptr<FuncDecl>> funcs;
  std::vector<std::unique_ptr<ConstDecl>> consts;
  std::vector<std::unique_ptr<WhenDecl>> whens; // expanded away by sema
  // Filled in by sema: predefined constants (OS, ARCH, -D ...) and
  // monomorphized generics.
  std::vector<std::unique_ptr<ConstDecl>> builtinConsts;
  std::vector<std::unique_ptr<StructDecl>> structInstances;
  std::vector<std::unique_ptr<FuncDecl>> funcInstances;
};

// Top-level compile-time `if` selecting declarations.
struct WhenDecl {
  SourceLoc loc;
  ExprPtr cond;
  Module thenItems;
  Module elseItems; // `else when` is a single nested WhenDecl
};

// Deep copies of syntax, without any sema annotations (for generics).
std::unique_ptr<TypeRef> cloneTypeRef(const TypeRef &t);
ExprPtr cloneExpr(const Expr &e);
StmtPtr cloneStmt(const Stmt &s);

void dumpModule(const Module &m);

} // namespace hoshi
