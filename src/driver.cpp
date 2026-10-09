#include "driver.h"

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <cstdlib>
#include <memory>

namespace hoshi {

namespace {

int fail(const std::string &msg) {
  llvm::errs() << "hoshic: error: " << msg << "\n";
  return 1;
}

std::unique_ptr<llvm::TargetMachine> createTargetMachine(const Options &opts,
                                                         std::string &err) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, err);
  if (!target)
    return nullptr;

  std::string cpu = "generic";
  std::string features;
  if (opts.nativeCpu) {
    cpu = llvm::sys::getHostCPUName().str();
    for (auto &[name, enabled] : llvm::sys::getHostCPUFeatures())
      features += (features.empty() ? "" : ",") + std::string(enabled ? "+" : "-") +
                  name.str();
  } else if (triple.getArch() == llvm::Triple::x86_64) {
    cpu = "x86-64"; // same baseline clang uses by default
  }

  llvm::CodeGenOptLevel level = opts.optLevel == 0   ? llvm::CodeGenOptLevel::None
                                : opts.optLevel == 1 ? llvm::CodeGenOptLevel::Less
                                : opts.optLevel == 2 ? llvm::CodeGenOptLevel::Default
                                                     : llvm::CodeGenOptLevel::Aggressive;
  llvm::TargetOptions to;
  return std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      triple, cpu, features, to, llvm::Reloc::PIC_, std::nullopt, level));
}

void optimize(llvm::Module &mod, llvm::TargetMachine &tm, unsigned level) {
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;

  llvm::PassBuilder pb(&tm);
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  llvm::OptimizationLevel ol = level == 1   ? llvm::OptimizationLevel::O1
                               : level == 2 ? llvm::OptimizationLevel::O2
                                            : llvm::OptimizationLevel::O3;
  llvm::ModulePassManager mpm = level == 0
                                    ? pb.buildO0DefaultPipeline(llvm::OptimizationLevel::O0)
                                    : pb.buildPerModuleDefaultPipeline(ol);
  mpm.run(mod, mam);
}

bool emitMachineCode(llvm::Module &mod, llvm::TargetMachine &tm, const std::string &path,
                     llvm::CodeGenFileType kind, std::string &err) {
  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    err = "cannot open '" + path + "': " + ec.message();
    return false;
  }
  llvm::legacy::PassManager pm;
  if (tm.addPassesToEmitFile(pm, out, nullptr, kind)) {
    err = "target cannot emit this file type";
    return false;
  }
  pm.run(mod);
  out.flush();
  return true;
}

// Link with the system C compiler driver so crt files and libc are found the
// same way C programs find them. Override with $HOSHI_CC.
int link(const std::string &object, const Options &opts, const std::string &output) {
  std::string ccName;
  if (const char *env = std::getenv("HOSHI_CC"))
    ccName = env;
  llvm::ErrorOr<std::string> cc = std::make_error_code(std::errc::no_such_file_or_directory);
  for (const std::string &candidate :
       ccName.empty() ? std::vector<std::string>{"clang", "cc", "gcc"}
                      : std::vector<std::string>{ccName}) {
    cc = llvm::sys::findProgramByName(candidate);
    if (cc)
      break;
  }
  if (!cc)
    return fail("no C compiler found to link with (tried clang, cc, gcc; set HOSHI_CC)");

  std::vector<std::string> args = {*cc, object, "-o", output, "-lm"};
  args.insert(args.end(), opts.linkArgs.begin(), opts.linkArgs.end());
  std::vector<llvm::StringRef> refs(args.begin(), args.end());

  std::string err;
  int rc = llvm::sys::ExecuteAndWait(*cc, refs, std::nullopt, {}, 0, 0, &err);
  if (rc != 0)
    return fail("linking failed" + (err.empty() ? "" : ": " + err));
  return 0;
}

std::string defaultOutput(const Options &opts) {
  llvm::SmallString<128> path(llvm::sys::path::filename(opts.input));
  switch (opts.emit) {
  case EmitKind::Executable: llvm::sys::path::replace_extension(path, ""); break;
  case EmitKind::Object: llvm::sys::path::replace_extension(path, "o"); break;
  case EmitKind::Assembly: llvm::sys::path::replace_extension(path, "s"); break;
  case EmitKind::LLVMIR: return "-";
  case EmitKind::AST: return "-";
  }
  if (path.empty() || path == opts.input)
    path = "a.out";
  return std::string(path);
}

} // namespace

int compile(const Options &opts) {
  auto buf = llvm::MemoryBuffer::getFile(opts.input);
  if (!buf)
    return fail("cannot read '" + opts.input + "': " + buf.getError().message());
  SourceFile file{opts.input, (*buf)->getBuffer().str()};
  std::string output = opts.output.empty() ? defaultOutput(opts) : opts.output;

  hoshi::Module ast;
  TypeContext types;
  try {
    ast = parse(lex(file));
    if (opts.emit == EmitKind::AST) {
      dumpModule(ast);
      return 0;
    }
    analyze(ast, types);
  } catch (const CompileError &e) {
    printError(file, e);
    return 1;
  }

  std::string err;
  auto tm = createTargetMachine(opts, err);
  if (!tm)
    return fail("cannot create target machine: " + err);

  llvm::LLVMContext ctx;
  llvm::Module mod(llvm::sys::path::filename(opts.input), ctx);
  mod.setSourceFileName(opts.input);
  mod.setTargetTriple(tm->getTargetTriple());
  mod.setDataLayout(tm->createDataLayout());
  generate(ast, types, mod);

  if (llvm::verifyModule(mod, &llvm::errs())) {
    llvm::errs() << "hoshic: internal error: generated invalid LLVM IR\n";
    return 2;
  }
  optimize(mod, *tm, opts.optLevel);

  switch (opts.emit) {
  case EmitKind::LLVMIR: {
    std::error_code ec;
    llvm::raw_fd_ostream out(output, ec, llvm::sys::fs::OF_Text);
    if (ec)
      return fail("cannot open '" + output + "': " + ec.message());
    mod.print(out, nullptr);
    return 0;
  }
  case EmitKind::Assembly:
  case EmitKind::Object: {
    auto kind = opts.emit == EmitKind::Object ? llvm::CodeGenFileType::ObjectFile
                                              : llvm::CodeGenFileType::AssemblyFile;
    if (!emitMachineCode(mod, *tm, output, kind, err))
      return fail(err);
    return 0;
  }
  case EmitKind::Executable: {
    llvm::SmallString<128> obj;
    if (auto ec = llvm::sys::fs::createTemporaryFile("hoshi", "o", obj))
      return fail("cannot create temporary file: " + ec.message());
    std::string objPath(obj);
    int rc = emitMachineCode(mod, *tm, objPath, llvm::CodeGenFileType::ObjectFile, err)
                 ? link(objPath, opts, output)
                 : fail(err);
    llvm::sys::fs::remove(objPath);
    return rc;
  }
  case EmitKind::AST:
    break;
  }
  return 0;
}

} // namespace hoshi
