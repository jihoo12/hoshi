#pragma once

#include <string>
#include <vector>

namespace hoshi {

enum class EmitKind { Executable, Object, Assembly, LLVMIR, AST };

struct Options {
  std::string input;
  std::string output; // empty: derive from input
  EmitKind emit = EmitKind::Executable;
  unsigned optLevel = 0;
  bool nativeCpu = false;
  std::vector<std::string> linkArgs; // passed through to the linker (-l, -L)
};

// Returns the process exit code.
int compile(const Options &opts);

} // namespace hoshi
