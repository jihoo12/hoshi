#include "diag.h"

#include <cstdio>
#include <unistd.h>

namespace hoshi {

std::string SourceFile::lineText(unsigned line) const {
  size_t start = 0;
  for (unsigned i = 1; i < line && start != std::string::npos; ++i) {
    start = text.find('\n', start);
    if (start != std::string::npos)
      ++start;
  }
  if (start == std::string::npos || start > text.size())
    return "";
  size_t end = text.find('\n', start);
  return text.substr(start, end == std::string::npos ? std::string::npos
                                                      : end - start);
}

void error(SourceLoc loc, const std::string &msg) { throw CompileError(loc, msg); }

static void printDiag(const SourceFile &file, SourceLoc loc, const char *kind,
                      const std::string &msg) {
  bool color = isatty(fileno(stderr));
  const char *bold = color ? "\033[1m" : "";
  const char *kindColor = !color ? "" : kind[0] == 'e' ? "\033[1;31m" : "\033[1;36m";
  const char *green = color ? "\033[1;32m" : "";
  const char *reset = color ? "\033[0m" : "";

  std::fprintf(stderr, "%s%s:%u:%u: %s%s:%s %s%s%s\n", bold, file.path.c_str(), loc.line,
               loc.col, kindColor, kind, reset, bold, msg.c_str(), reset);
  if (loc.line == 0)
    return;

  std::string line = file.lineText(loc.line);
  std::fprintf(stderr, "%5u | %s\n      | ", loc.line, line.c_str());
  for (unsigned i = 1; i < loc.col; ++i)
    std::fputc(i - 1 < line.size() && line[i - 1] == '\t' ? '\t' : ' ', stderr);
  std::fprintf(stderr, "%s^%s\n", green, reset);
}

void printError(const SourceFile &file, const CompileError &err) {
  printDiag(file, err.loc, "error", err.what());
  for (auto &[loc, msg] : err.notes)
    printDiag(file, loc, "note", msg);
}

} // namespace hoshi
