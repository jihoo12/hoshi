#include "codegen.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include <unordered_map>

namespace hoshi {

namespace {

class CodeGen {
public:
  CodeGen(llvm::Module &mod)
      : mod(mod), ctx(mod.getContext()), b(ctx), dl(mod.getDataLayout()) {}

  void run(Module &m) {
    // Create all struct types first so fields can refer to any of them.
    for (auto &s : m.structs)
      structTys[s.get()] = llvm::StructType::create(ctx, "struct." + s->name);
    for (auto &s : m.structs) {
      std::vector<llvm::Type *> fields;
      for (auto &f : s->fields)
        fields.push_back(llvmType(f.type));
      structTys[s.get()]->setBody(fields);
    }
    for (auto &f : m.funcs)
      declareFunc(*f);
    for (auto &f : m.funcs)
      if (f->body)
        genFunc(*f);
  }

private:
  llvm::Module &mod;
  llvm::LLVMContext &ctx;
  llvm::IRBuilder<> b;
  const llvm::DataLayout &dl;
  std::unordered_map<StructDecl *, llvm::StructType *> structTys;

  FuncDecl *curDecl = nullptr;
  llvm::Function *curFn = nullptr;

  struct LoopTarget {
    llvm::BasicBlock *breakBB, *continueBB;
    size_t deferDepth; // number of defer scopes live at the loop
  };
  std::vector<LoopTarget> loops;
  std::vector<std::vector<Stmt *>> deferScopes;

  // ---- Types --------------------------------------------------------------

  llvm::Type *llvmType(Type *t) {
    switch (t->kind) {
    case TypeKind::Void: return b.getVoidTy();
    case TypeKind::Bool: return b.getInt1Ty();
    case TypeKind::Int: return b.getIntNTy(t->bits);
    case TypeKind::Float: return t->bits == 32 ? b.getFloatTy() : b.getDoubleTy();
    case TypeKind::Pointer: return b.getPtrTy();
    case TypeKind::Array: return llvm::ArrayType::get(llvmType(t->elem), t->count);
    case TypeKind::Struct: return structTys.at(t->decl);
    }
    return nullptr;
  }

  // ---- Functions ----------------------------------------------------------

  bool isVoidMain(const FuncDecl &f) const {
    return f.name == "main" && !f.isExtern && f.retType->isVoid();
  }

  void declareFunc(FuncDecl &f) {
    std::vector<llvm::Type *> params;
    for (auto &p : f.params)
      params.push_back(llvmType(p.var.type));
    // A `fn main()` with no return type still returns 0 to the C runtime.
    llvm::Type *ret = isVoidMain(f) ? b.getInt32Ty() : llvmType(f.retType);
    auto *fty = llvm::FunctionType::get(ret, params, f.isVariadic);
    auto *fn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, f.name, mod);
    fn->addFnAttr(llvm::Attribute::NoUnwind); // Hoshi has no exceptions
    for (size_t i = 0; i < f.params.size(); ++i)
      fn->getArg(i)->setName(f.params[i].var.name);
    f.llvmFn = fn;
  }

  void genFunc(FuncDecl &f) {
    curDecl = &f;
    curFn = f.llvmFn;
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", curFn));

    for (size_t i = 0; i < f.params.size(); ++i) {
      VarSym &v = f.params[i].var;
      v.addr = entryAlloca(llvmType(v.type), v.name);
      b.CreateStore(curFn->getArg(i), v.addr);
    }

    deferScopes.clear();
    loops.clear();
    genBlock(*f.body);

    // Falling off the end of the function.
    if (!b.GetInsertBlock()->getTerminator()) {
      if (isVoidMain(f))
        b.CreateRet(b.getInt32(0));
      else if (f.retType->isVoid())
        b.CreateRetVoid();
      else if (f.name == "main")
        b.CreateRet(b.getInt32(0));
      else
        b.CreateUnreachable(); // sema proved every path returns
    }
    curFn = nullptr;
    curDecl = nullptr;
  }

  // All allocas go in the entry block so mem2reg can promote them.
  llvm::AllocaInst *entryAlloca(llvm::Type *ty, const std::string &name) {
    llvm::BasicBlock &entry = curFn->getEntryBlock();
    llvm::IRBuilder<> tmp(&entry, entry.begin());
    return tmp.CreateAlloca(ty, nullptr, name);
  }

  // After a terminator, keep emitting into a fresh block with no predecessors.
  // LLVM removes such blocks; this keeps codegen free of special cases.
  void startDeadBlock() {
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "dead", curFn));
  }

  // ---- Statements ---------------------------------------------------------

  void genBlock(BlockStmt &blk) {
    deferScopes.emplace_back();
    for (auto &s : blk.stmts)
      genStmt(*s);
    emitDefers(deferScopes.size() - 1);
    deferScopes.pop_back();
  }

  // Run the deferred statements of every scope at index >= `depth`, innermost
  // first and, within a scope, in reverse order of declaration.
  void emitDefers(size_t depth) {
    for (size_t i = deferScopes.size(); i-- > depth;) {
      std::vector<Stmt *> list = deferScopes[i]; // genStmt may grow deferScopes
      for (auto it = list.rbegin(); it != list.rend(); ++it)
        genStmt(**it);
    }
  }

  void genStmt(Stmt &s) {
    switch (s.kind) {
    case StmtKind::Block:
      genBlock(static_cast<BlockStmt &>(s));
      return;
    case StmtKind::Let: {
      auto &l = static_cast<LetStmt &>(s);
      llvm::Type *ty = llvmType(l.var.type);
      llvm::Value *init = l.init ? genExpr(*l.init) : llvm::Constant::getNullValue(ty);
      l.var.addr = entryAlloca(ty, l.var.name);
      b.CreateStore(init, l.var.addr);
      return;
    }
    case StmtKind::Expr:
      genExpr(*static_cast<ExprStmt &>(s).expr);
      return;
    case StmtKind::Assign: {
      auto &a = static_cast<AssignStmt &>(s);
      llvm::Value *addr = genAddr(*a.target);
      llvm::Value *val = genExpr(*a.value);
      if (a.compound) {
        llvm::Value *cur = b.CreateLoad(llvmType(a.target->type), addr);
        val = genBinOp(a.op, cur, val, a.target->type, a.value->type);
      }
      b.CreateStore(val, addr);
      return;
    }
    case StmtKind::If: {
      auto &i = static_cast<IfStmt &>(s);
      llvm::Value *cond = genExpr(*i.cond);
      auto *thenBB = llvm::BasicBlock::Create(ctx, "if.then", curFn);
      auto *mergeBB = llvm::BasicBlock::Create(ctx, "if.end");
      auto *elseBB = i.elseStmt ? llvm::BasicBlock::Create(ctx, "if.else") : mergeBB;
      b.CreateCondBr(cond, thenBB, elseBB);

      b.SetInsertPoint(thenBB);
      genBlock(*i.thenBlock);
      b.CreateBr(mergeBB);

      if (i.elseStmt) {
        elseBB->insertInto(curFn);
        b.SetInsertPoint(elseBB);
        genStmt(*i.elseStmt);
        b.CreateBr(mergeBB);
      }
      mergeBB->insertInto(curFn);
      b.SetInsertPoint(mergeBB);
      return;
    }
    case StmtKind::While: {
      auto &w = static_cast<WhileStmt &>(s);
      auto *condBB = llvm::BasicBlock::Create(ctx, "while.cond", curFn);
      auto *bodyBB = llvm::BasicBlock::Create(ctx, "while.body");
      auto *endBB = llvm::BasicBlock::Create(ctx, "while.end");
      b.CreateBr(condBB);

      b.SetInsertPoint(condBB);
      b.CreateCondBr(genExpr(*w.cond), bodyBB, endBB);

      bodyBB->insertInto(curFn);
      b.SetInsertPoint(bodyBB);
      loops.push_back({endBB, condBB, deferScopes.size()});
      genBlock(*w.body);
      loops.pop_back();
      b.CreateBr(condBB);

      endBB->insertInto(curFn);
      b.SetInsertPoint(endBB);
      return;
    }
    case StmtKind::For: {
      auto &f = static_cast<ForStmt &>(s);
      Type *ty = f.var.type;
      llvm::Type *lty = llvmType(ty);
      llvm::Value *start = genExpr(*f.start);
      llvm::Value *end = genExpr(*f.end); // evaluated once
      f.var.addr = entryAlloca(lty, f.var.name);
      b.CreateStore(start, f.var.addr);

      auto *condBB = llvm::BasicBlock::Create(ctx, "for.cond", curFn);
      auto *bodyBB = llvm::BasicBlock::Create(ctx, "for.body");
      auto *incBB = llvm::BasicBlock::Create(ctx, "for.inc");
      auto *endBB = llvm::BasicBlock::Create(ctx, "for.end");
      b.CreateBr(condBB);

      b.SetInsertPoint(condBB);
      llvm::Value *iv = b.CreateLoad(lty, f.var.addr);
      llvm::Value *cond = ty->isSigned ? b.CreateICmpSLT(iv, end) : b.CreateICmpULT(iv, end);
      b.CreateCondBr(cond, bodyBB, endBB);

      bodyBB->insertInto(curFn);
      b.SetInsertPoint(bodyBB);
      loops.push_back({endBB, incBB, deferScopes.size()});
      genBlock(*f.body);
      loops.pop_back();
      b.CreateBr(incBB);

      incBB->insertInto(curFn);
      b.SetInsertPoint(incBB);
      // i < end held before the increment, so i + 1 cannot overflow.
      llvm::Value *next = b.CreateAdd(b.CreateLoad(lty, f.var.addr),
                                      llvm::ConstantInt::get(lty, 1), "", !ty->isSigned,
                                      ty->isSigned);
      b.CreateStore(next, f.var.addr);
      b.CreateBr(condBB);

      endBB->insertInto(curFn);
      b.SetInsertPoint(endBB);
      return;
    }
    case StmtKind::Return: {
      auto &r = static_cast<ReturnStmt &>(s);
      // The return value is computed before deferred statements run.
      llvm::Value *v = r.value ? genExpr(*r.value) : nullptr;
      emitDefers(0);
      if (v)
        b.CreateRet(v);
      else if (isVoidMain(*curDecl))
        b.CreateRet(b.getInt32(0));
      else
        b.CreateRetVoid();
      startDeadBlock();
      return;
    }
    case StmtKind::Break:
    case StmtKind::Continue: {
      LoopTarget &t = loops.back();
      emitDefers(t.deferDepth);
      b.CreateBr(s.kind == StmtKind::Break ? t.breakBB : t.continueBB);
      startDeadBlock();
      return;
    }
    case StmtKind::Defer:
      deferScopes.back().push_back(static_cast<DeferStmt &>(s).body.get());
      return;
    }
  }

  // ---- Expressions --------------------------------------------------------

  llvm::Value *genExpr(Expr &e) {
    switch (e.kind) {
    case ExprKind::IntLit: {
      auto &i = static_cast<IntLitExpr &>(e);
      if (e.type->isFloat())
        return llvm::ConstantFP::get(llvmType(e.type), (double)i.value);
      return llvm::ConstantInt::get(llvmType(e.type), i.value);
    }
    case ExprKind::FloatLit:
      return llvm::ConstantFP::get(llvmType(e.type), static_cast<FloatLitExpr &>(e).value);
    case ExprKind::BoolLit:
      return b.getInt1(static_cast<BoolLitExpr &>(e).value);
    case ExprKind::NullLit:
      return llvm::ConstantPointerNull::get(b.getPtrTy());
    case ExprKind::StringLit:
      return b.CreateGlobalString(static_cast<StringLitExpr &>(e).value, ".str");
    case ExprKind::Ident: {
      auto &id = static_cast<IdentExpr &>(e);
      return b.CreateLoad(llvmType(e.type), id.var->addr, id.name);
    }
    case ExprKind::Unary:
      return genUnary(static_cast<UnaryExpr &>(e));
    case ExprKind::Binary: {
      auto &bin = static_cast<BinaryExpr &>(e);
      if (bin.op == BinaryOp::And || bin.op == BinaryOp::Or)
        return genLogical(bin);
      llvm::Value *l = genExpr(*bin.lhs);
      llvm::Value *r = genExpr(*bin.rhs);
      return genBinOp(bin.op, l, r, bin.lhs->type, bin.rhs->type);
    }
    case ExprKind::Call:
      return genCall(static_cast<CallExpr &>(e));
    case ExprKind::Member:
    case ExprKind::Index:
      return b.CreateLoad(llvmType(e.type), genAddr(e));
    case ExprKind::Cast: {
      auto &c = static_cast<CastExpr &>(e);
      return genCast(genExpr(*c.operand), c.operand->type, c.type);
    }
    case ExprKind::SizeOf: {
      auto &so = static_cast<SizeOfExpr &>(e);
      uint64_t size = dl.getTypeAllocSize(llvmType(so.target->resolved));
      return b.getInt64(size);
    }
    case ExprKind::StructLit: {
      auto &s = static_cast<StructLitExpr &>(e);
      llvm::Value *agg = llvm::Constant::getNullValue(llvmType(e.type));
      for (auto &f : s.fields)
        agg = b.CreateInsertValue(agg, genExpr(*f.value), f.index);
      return agg;
    }
    case ExprKind::ArrayLit: {
      auto &a = static_cast<ArrayLitExpr &>(e);
      llvm::Value *agg = llvm::Constant::getNullValue(llvmType(e.type));
      for (unsigned i = 0; i < a.elems.size(); ++i)
        agg = b.CreateInsertValue(agg, genExpr(*a.elems[i]), i);
      return agg;
    }
    }
    return nullptr;
  }

  // Address of an lvalue. Temporaries are spilled to a stack slot so that
  // `make_point().x` and `make_array()[i]` work too.
  llvm::Value *genAddr(Expr &e) {
    switch (e.kind) {
    case ExprKind::Ident:
      return static_cast<IdentExpr &>(e).var->addr;
    case ExprKind::Unary: {
      auto &u = static_cast<UnaryExpr &>(e);
      if (u.op == UnaryOp::Deref)
        return genExpr(*u.operand);
      break;
    }
    case ExprKind::Member: {
      auto &m = static_cast<MemberExpr &>(e);
      llvm::Value *base = m.throughPointer ? genExpr(*m.base) : genAddr(*m.base);
      StructDecl *decl = m.throughPointer ? m.base->type->elem->decl : m.base->type->decl;
      return b.CreateStructGEP(structTys.at(decl), base, m.fieldIndex, m.field);
    }
    case ExprKind::Index: {
      auto &ix = static_cast<IndexExpr &>(e);
      llvm::Value *idx = toIndex(genExpr(*ix.index), ix.index->type);
      if (ix.base->type->isPointer())
        return b.CreateInBoundsGEP(llvmType(e.type), genExpr(*ix.base), idx);
      llvm::Value *arr = genAddr(*ix.base);
      return b.CreateInBoundsGEP(llvmType(ix.base->type), arr, {b.getInt64(0), idx});
    }
    default:
      break;
    }
    llvm::Value *tmp = entryAlloca(llvmType(e.type), "tmp");
    b.CreateStore(genExpr(e), tmp);
    return tmp;
  }

  llvm::Value *toIndex(llvm::Value *v, Type *t) {
    return b.CreateIntCast(v, b.getInt64Ty(), t->isSigned);
  }

  llvm::Value *genUnary(UnaryExpr &u) {
    switch (u.op) {
    case UnaryOp::Neg: {
      llvm::Value *v = genExpr(*u.operand);
      return u.type->isFloat() ? b.CreateFNeg(v) : b.CreateNeg(v);
    }
    case UnaryOp::Not:
    case UnaryOp::BitNot:
      return b.CreateNot(genExpr(*u.operand));
    case UnaryOp::AddrOf:
      return genAddr(*u.operand);
    case UnaryOp::Deref:
      return b.CreateLoad(llvmType(u.type), genExpr(*u.operand));
    }
    return nullptr;
  }

  llvm::Value *genLogical(BinaryExpr &bin) {
    bool isAnd = bin.op == BinaryOp::And;
    llvm::Value *l = genExpr(*bin.lhs);
    llvm::BasicBlock *lhsBB = b.GetInsertBlock();
    auto *rhsBB = llvm::BasicBlock::Create(ctx, isAnd ? "and.rhs" : "or.rhs", curFn);
    auto *endBB = llvm::BasicBlock::Create(ctx, isAnd ? "and.end" : "or.end");
    if (isAnd)
      b.CreateCondBr(l, rhsBB, endBB);
    else
      b.CreateCondBr(l, endBB, rhsBB);

    b.SetInsertPoint(rhsBB);
    llvm::Value *r = genExpr(*bin.rhs);
    rhsBB = b.GetInsertBlock(); // rhs may have created blocks
    b.CreateBr(endBB);

    endBB->insertInto(curFn);
    b.SetInsertPoint(endBB);
    auto *phi = b.CreatePHI(b.getInt1Ty(), 2);
    phi->addIncoming(b.getInt1(!isAnd), lhsBB);
    phi->addIncoming(r, rhsBB);
    return phi;
  }

  llvm::Value *genBinOp(BinaryOp op, llvm::Value *l, llvm::Value *r, Type *lt, Type *rt) {
    if (lt->isPointer() && rt->isInt()) {
      llvm::Value *idx = toIndex(r, rt);
      if (op == BinaryOp::Sub)
        idx = b.CreateNeg(idx);
      return b.CreateInBoundsGEP(llvmType(lt->elem), l, idx);
    }
    if (lt->isPointer() && op == BinaryOp::Sub)
      return b.CreatePtrDiff(llvmType(lt->elem), l, r);

    if (lt->isFloat()) {
      switch (op) {
      case BinaryOp::Add: return b.CreateFAdd(l, r);
      case BinaryOp::Sub: return b.CreateFSub(l, r);
      case BinaryOp::Mul: return b.CreateFMul(l, r);
      case BinaryOp::Div: return b.CreateFDiv(l, r);
      case BinaryOp::Rem: return b.CreateFRem(l, r);
      case BinaryOp::Eq: return b.CreateFCmpOEQ(l, r);
      case BinaryOp::Ne: return b.CreateFCmpUNE(l, r);
      case BinaryOp::Lt: return b.CreateFCmpOLT(l, r);
      case BinaryOp::Le: return b.CreateFCmpOLE(l, r);
      case BinaryOp::Gt: return b.CreateFCmpOGT(l, r);
      case BinaryOp::Ge: return b.CreateFCmpOGE(l, r);
      default: break;
      }
    }

    // Integers, bools and pointer comparisons. As in C, signed overflow is
    // undefined (nsw), which lets LLVM optimize loops the way clang does.
    bool s = lt->isInt() && lt->isSigned;
    switch (op) {
    case BinaryOp::Add: return b.CreateAdd(l, r, "", false, s);
    case BinaryOp::Sub: return b.CreateSub(l, r, "", false, s);
    case BinaryOp::Mul: return b.CreateMul(l, r, "", false, s);
    case BinaryOp::Div: return s ? b.CreateSDiv(l, r) : b.CreateUDiv(l, r);
    case BinaryOp::Rem: return s ? b.CreateSRem(l, r) : b.CreateURem(l, r);
    case BinaryOp::BitAnd: return b.CreateAnd(l, r);
    case BinaryOp::BitOr: return b.CreateOr(l, r);
    case BinaryOp::BitXor: return b.CreateXor(l, r);
    case BinaryOp::Shl: return b.CreateShl(l, r);
    case BinaryOp::Shr: return s ? b.CreateAShr(l, r) : b.CreateLShr(l, r);
    case BinaryOp::Eq: return b.CreateICmpEQ(l, r);
    case BinaryOp::Ne: return b.CreateICmpNE(l, r);
    case BinaryOp::Lt: return s ? b.CreateICmpSLT(l, r) : b.CreateICmpULT(l, r);
    case BinaryOp::Le: return s ? b.CreateICmpSLE(l, r) : b.CreateICmpULE(l, r);
    case BinaryOp::Gt: return s ? b.CreateICmpSGT(l, r) : b.CreateICmpUGT(l, r);
    case BinaryOp::Ge: return s ? b.CreateICmpSGE(l, r) : b.CreateICmpUGE(l, r);
    case BinaryOp::And:
    case BinaryOp::Or: break; // handled by genLogical
    }
    return nullptr;
  }

  llvm::Value *genCall(CallExpr &c) {
    FuncDecl *fn = c.fn;
    std::vector<llvm::Value *> args;
    for (size_t i = 0; i < c.args.size(); ++i) {
      llvm::Value *v = genExpr(*c.args[i]);
      if (i >= fn->params.size())
        v = promoteVariadic(v, c.args[i]->type);
      args.push_back(v);
    }
    return b.CreateCall(fn->llvmFn, args);
  }

  // C default argument promotions for `...` arguments.
  llvm::Value *promoteVariadic(llvm::Value *v, Type *t) {
    if (t->isFloat() && t->bits < 64)
      return b.CreateFPExt(v, b.getDoubleTy());
    if (t->isBool())
      return b.CreateZExt(v, b.getInt32Ty());
    if (t->isInt() && t->bits < 32)
      return b.CreateIntCast(v, b.getInt32Ty(), t->isSigned);
    return v;
  }

  llvm::Value *genCast(llvm::Value *v, Type *from, Type *to) {
    if (from == to)
      return v;
    llvm::Type *lto = llvmType(to);
    if (from->isInt() && to->isInt())
      return b.CreateIntCast(v, lto, from->isSigned);
    if (from->isInt() && to->isFloat())
      return from->isSigned ? b.CreateSIToFP(v, lto) : b.CreateUIToFP(v, lto);
    if (from->isFloat() && to->isInt())
      return to->isSigned ? b.CreateFPToSI(v, lto) : b.CreateFPToUI(v, lto);
    if (from->isFloat() && to->isFloat())
      return b.CreateFPCast(v, lto);
    if (from->isBool() && to->isInt())
      return b.CreateZExt(v, lto);
    if (from->isPointer() && to->isPointer())
      return v;
    if (from->isPointer() && to->isInt())
      return b.CreatePtrToInt(v, lto);
    if (from->isInt() && to->isPointer())
      return b.CreateIntToPtr(v, lto);
    return v; // sema rejects everything else
  }
};

} // namespace

void generate(Module &m, TypeContext &types, llvm::Module &out) {
  CodeGen(out).run(m);
}

} // namespace hoshi
