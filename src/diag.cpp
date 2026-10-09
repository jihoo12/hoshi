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

void printError(const SourceFile &file, const CompileError &err) {
  bool color = isatty(fileno(stderr));
  const char *bold = color ? "\033[1m" : "";
  const char *red = color ? "\033[1;31m" : "";
  const char *green = color ? "\033[1;32m" : "";
  const char *reset = color ? "\033[0m" : "";

  std::fprintf(stderr, "%s%s:%u:%u: %serror:%s %s%s\n", bold, file.path.c_str(),
               err.loc.line, err.loc.col, red, reset, bold, err.what());
  std::fputs(reset, stderr);
  if (err.loc.line == 0)
    return;

  std::string line = file.lineText(err.loc.line);
  std::fprintf(stderr, "%5u | %s\n      | ", err.loc.line, line.c_str());
  for (unsigned i = 1; i < err.loc.col; ++i)
    std::fputc(i - 1 < line.size() && line[i - 1] == '\t' ? '\t' : ' ', stderr);
  std::fprintf(stderr, "%s^%s\n", green, reset);
}

} // namespace hoshi
