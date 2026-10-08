# hoshi

A systems programming language that aims to combine simple, easy-to-learn syntax with C-like performance and low-level control.
The `hoshic` compiler is written in C++20 and uses LLVM.

```hoshi
extern fn printf(fmt: *u8, ...) -> i32;
extern fn malloc(size: u64) -> *void;
extern fn free(p: *void);

struct Vec2 { x: f64, y: f64 }

fn len2(v: *Vec2) -> f64 {
    return v.x * v.x + v.y * v.y;
}

fn main() {
    let buf = malloc(16 * sizeof(Vec2)) as *Vec2;
    defer free(buf as *void);

    for i in 0..16 {
        buf[i] = Vec2 { x: i as f64, y: 1.0 };
    }
    printf("%f\n", len2(&buf[3]));
}
```

## Design Principles

- **C-like semantics**: Value types, pointers, and manual memory management. No runtime, garbage collector, exceptions, or hidden allocations.
- **C ABI compatibility**: Call C functions, including libc functions, directly with `extern fn`. No name mangling. Structs and arrays cross the C boundary by pointer only (by-value aggregates in `extern fn` signatures are rejected until target ABI lowering is implemented).
- **No implicit conversions**: All type conversions must be explicit using `as`. Only integer literals have their types determined by context.
- **Optimization via LLVM**: Keep the frontend simple and use the same `-O2`/`-O3` optimization pipelines as Clang.
  Signed integer overflow is undefined behavior, as in C.

## Language Overview (v0)

| Category | Description |
|---|---|
| Types | `i8 i16 i32 i64 u8 u16 u32 u64 isize usize f32 f64 bool void`, `*T`, `[N]T`, `struct` |
| Variables | `let` (immutable), `var` (mutable; zero-initialized if no initializer is provided), type inference |
| Control flow | `if`/`else if`/`else`, `while`, `for i in a..b` (excluding b), `break`, `continue`, `return` |
| Cleanup | `defer stmt;` / `defer { ... }` — executed in reverse order on scope exit (including `return`/`break`) |
| Expressions | Arithmetic, bitwise, comparison, and logical (short-circuiting) operators; `&x`, `*p`, `p[i]`, `p + n`, `p - q`, `s.f` (automatic dereferencing for pointers), `as`, `sizeof(T)` |
| Literals | `42`, `0xff`, `0b1010`, `1_000`, `3.14`, `'a'` (u8), `"str"` (`*u8`, null-terminated), `true`, `false`, `null`, `[1, 2, 3]`, `Point { x: 1, y: 2 }` |
| main | `fn main()`, `fn main() -> i32`, `fn main(argc: i32, argv: **u8) -> i32` |

Operator precedence is similar to C, but bitwise operators (`& | ^`) bind more tightly than comparison operators (as in Go and Rust).

## Development Environment

Use the [Nix flake](https://nixos.wiki/wiki/Flakes) to enter a development environment with LLVM 21, Clang, CMake, Ninja, clangd, and more.

```sh
nix develop          # Or use direnv: `direnv allow` (.envrc included)
cmake -B build -G Ninja
ninja -C build

./build/hoshic examples/hello.hoshi -o hello && ./hello
python3 tests/run.py build/hoshic   # Golden tests (run at both -O0 and -O2)
bench/compare.sh                   # Compare performance with the equivalent C program (clang -O2)
```

Build the package (including tests) with `nix build` or `nix flake check`. The resulting `result/bin/hoshic` automatically adds Clang to `PATH` for linking.

## Using hoshic

```
hoshic [options] <file.hoshi>
  -o <path>        Output path
  -O0 .. -O3       Optimization level (default: -O0)
  -c / -S          Emit an object file / assembly
  --emit-llvm      Emit LLVM IR
  --dump-ast       Dump the parsed AST
  --march=native   Optimize for the current CPU
  -l<lib> -L<dir>  Pass options to the linker
```

Executables are linked with a system C compiler driver (tried in this order: `clang` → `cc` → `gcc`; configurable via `$HOSHI_CC`).

## Project Structure

```
src/lexer.*      Tokenization
src/parser.*     Recursive descent + precedence-based expression parsing → AST (src/ast.h)
src/sema.*       Name resolution, type checking, mutability/return-path checks
src/codegen.*    AST → LLVM IR (local variables: alloca → mem2reg for SSA)
src/driver.*     Optimization pipeline, object generation, linking
tests/cases      Golden tests for execution results (// expect:, // exit:)
tests/errors     Compilation error tests (// error:)
examples/        hello, fib, nbody
bench/           Benchmarks comparing against C
```

## Roadmap

- Modules/imports, enums and tagged unions, slices (`[]T`), global variables
- Debug information (DWARF), reporting multiple errors at once
- Function pointers, exploring generics
