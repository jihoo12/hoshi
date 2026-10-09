#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hoshi {

struct SourceLoc {
  unsigned line = 0;
  unsigned col = 0;
};

// A single source file kept in memory for the whole compilation.
struct SourceFile {
  std::string path;
  std::string text;

  std::string lineText(unsigned line) const;
};

// Thrown on the first error; the driver catches it and prints the diagnostic.
struct CompileError : std::runtime_error {
  SourceLoc loc;
  std::vector<std::pair<SourceLoc, std::string>> notes; // printed after the error
  CompileError(SourceLoc loc, std::string msg)
      : std::runtime_error(std::move(msg)), loc(loc) {}
};

[[noreturn]] void error(SourceLoc loc, const std::string &msg);

void printError(const SourceFile &file, const CompileError &err);

} // namespace hoshi
