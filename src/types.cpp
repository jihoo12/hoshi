#include "types.h"
#include "ast.h"

namespace hoshi {

std::string Type::str() const {
  switch (kind) {
  case TypeKind::Void: return "void";
  case TypeKind::Bool: return "bool";
  case TypeKind::Int: return (isSigned ? "i" : "u") + std::to_string(bits);
  case TypeKind::Float: return "f" + std::to_string(bits);
  case TypeKind::Pointer: return "*" + elem->str();
  case TypeKind::Array: return "[" + std::to_string(count) + "]" + elem->str();
  case TypeKind::Struct: return decl->name;
  }
  return "?";
}

TypeContext::TypeContext() {
  void_ = std::make_unique<Type>(Type{TypeKind::Void});
  bool_ = std::make_unique<Type>(Type{TypeKind::Bool});
  f32_ = std::make_unique<Type>(Type{TypeKind::Float, 32});
  f64_ = std::make_unique<Type>(Type{TypeKind::Float, 64});
}

Type *TypeContext::intTy(unsigned bits, bool isSigned) {
  auto &slot = ints_[{bits, isSigned}];
  if (!slot)
    slot = std::make_unique<Type>(Type{TypeKind::Int, bits, isSigned});
  return slot.get();
}

Type *TypeContext::pointerTo(Type *elem) {
  auto &slot = pointers_[elem];
  if (!slot) {
    slot = std::make_unique<Type>(Type{TypeKind::Pointer});
    slot->elem = elem;
  }
  return slot.get();
}

Type *TypeContext::arrayOf(Type *elem, uint64_t count) {
  auto &slot = arrays_[{elem, count}];
  if (!slot) {
    slot = std::make_unique<Type>(Type{TypeKind::Array});
    slot->elem = elem;
    slot->count = count;
  }
  return slot.get();
}

Type *TypeContext::structTy(StructDecl *decl) {
  auto &slot = structs_[decl];
  if (!slot) {
    slot = std::make_unique<Type>(Type{TypeKind::Struct});
    slot->decl = decl;
  }
  return slot.get();
}

Type *TypeContext::builtin(const std::string &name) {
  if (name == "void") return voidTy();
  if (name == "bool") return boolTy();
  if (name == "f32") return floatTy(32);
  if (name == "f64") return floatTy(64);
  if (name == "isize") return intTy(64, true);
  if (name == "usize") return intTy(64, false);
  if (name.size() >= 2 && (name[0] == 'i' || name[0] == 'u')) {
    std::string rest = name.substr(1);
    if (rest == "8" || rest == "16" || rest == "32" || rest == "64")
      return intTy(std::stoi(rest), name[0] == 'i');
  }
  return nullptr;
}

} // namespace hoshi
