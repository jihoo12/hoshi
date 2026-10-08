#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hoshi {

struct StructDecl;

enum class TypeKind { Void, Bool, Int, Float, Pointer, Array, Struct };

// Types are interned by TypeContext, so two types are equal iff their pointers
// are equal.
struct Type {
  TypeKind kind;
  unsigned bits = 0;        // Int, Float
  bool isSigned = false;    // Int
  Type *elem = nullptr;     // Pointer, Array
  uint64_t count = 0;       // Array
  StructDecl *decl = nullptr; // Struct

  bool isVoid() const { return kind == TypeKind::Void; }
  bool isBool() const { return kind == TypeKind::Bool; }
  bool isInt() const { return kind == TypeKind::Int; }
  bool isFloat() const { return kind == TypeKind::Float; }
  bool isNumeric() const { return isInt() || isFloat(); }
  bool isPointer() const { return kind == TypeKind::Pointer; }
  bool isArray() const { return kind == TypeKind::Array; }
  bool isStruct() const { return kind == TypeKind::Struct; }
  bool isScalar() const { return isBool() || isNumeric() || isPointer(); }

  std::string str() const;
};

class TypeContext {
public:
  TypeContext();

  Type *voidTy() { return void_.get(); }
  Type *boolTy() { return bool_.get(); }
  Type *intTy(unsigned bits, bool isSigned);
  Type *floatTy(unsigned bits) { return bits == 32 ? f32_.get() : f64_.get(); }
  Type *pointerTo(Type *elem);
  Type *arrayOf(Type *elem, uint64_t count);
  Type *structTy(StructDecl *decl);

  // Resolve builtin names such as "i32", "f64", "bool", "void".
  Type *builtin(const std::string &name);

private:
  std::unique_ptr<Type> void_, bool_, f32_, f64_;
  std::map<std::pair<unsigned, bool>, std::unique_ptr<Type>> ints_;
  std::map<Type *, std::unique_ptr<Type>> pointers_;
  std::map<std::pair<Type *, uint64_t>, std::unique_ptr<Type>> arrays_;
  std::map<StructDecl *, std::unique_ptr<Type>> structs_;
};

} // namespace hoshi
