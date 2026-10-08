#include "driver.h"

#include <cstdio>
#include <string>

namespace {

void usage() {
  std::puts(
      "usage: hoshic [options] <file.hoshi>\n"
      "\n"
      "options:\n"
      "  -o <path>       write output to <path>\n"
      "  -O0 -O1 -O2 -O3 optimization level (default -O0)\n"
      "  -c              emit an object file instead of an executable\n"
      "  -S              emit assembly\n"
      "  --emit-llvm     emit LLVM IR (to stdout unless -o is given)\n"
      "  --dump-ast      print the parsed syntax tree\n"
      "  --march=native  optimize for the host CPU\n"
      "  -l<lib> -L<dir> passed to the linker\n"
      "  --version       print version information\n"
      "  -h, --help      show this help");
}

} // namespace

int main(int argc, char **argv) {
  hoshi::Options opts;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (a == "--version") {
      std::printf("hoshic %s (LLVM %s)\n", HOSHI_VERSION, HOSHI_LLVM_VERSION);
      return 0;
    } else if (a == "-o") {
      if (++i >= argc) {
        std::fputs("hoshic: error: -o needs an argument\n", stderr);
        return 1;
      }
      opts.output = argv[i];
    } else if (a.size() == 3 && a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '3') {
      opts.optLevel = a[2] - '0';
    } else if (a == "-c") {
      opts.emit = hoshi::EmitKind::Object;
    } else if (a == "-S") {
      opts.emit = hoshi::EmitKind::Assembly;
    } else if (a == "--emit-llvm") {
      opts.emit = hoshi::EmitKind::LLVMIR;
    } else if (a == "--dump-ast") {
      opts.emit = hoshi::EmitKind::AST;
    } else if (a == "--march=native") {
      opts.nativeCpu = true;
    } else if (a.rfind("-l", 0) == 0 || a.rfind("-L", 0) == 0) {
      opts.linkArgs.push_back(a);
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "hoshic: error: unknown option '%s'\n", a.c_str());
      return 1;
    } else if (opts.input.empty()) {
      opts.input = a;
    } else {
      std::fputs("hoshic: error: only one input file is supported\n", stderr);
      return 1;
    }
  }
  if (opts.input.empty()) {
    usage();
    return 1;
  }
  return hoshi::compile(opts);
}
