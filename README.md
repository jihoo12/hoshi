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

## Language Overview

| Category | Description |
|---|---|
| Types | `i8 i16 i32 i64 u8 u16 u32 u64 isize usize f32 f64 bool void`, `*T`, `[N]T`, `struct` |
| Variables | `let` (immutable), `var` (mutable; zero-initialized if no initializer is provided), type inference |
| Control flow | `if`/`else if`/`else`, `while`, `for i in a..b` (excluding b), `break`, `continue`, `return` |
| Cleanup | `defer stmt;` / `defer { ... }` — executed in reverse order on scope exit (including `return`/`break`) |
| Expressions | Arithmetic, bitwise, comparison, and logical (short-circuiting) operators; `&x`, `*p`, `p[i]`, `p + n`, `p - q`, `s.f` (automatic dereferencing for pointers), `as`, `sizeof(T)` |
| Literals | `42`, `0xff`, `0b1010`, `1_000`, `3.14`, `'a'` (u8), `"str"` (`*u8`, null-terminated), `true`, `false`, `null`, `[1, 2, 3]`, `Point { x: 1, y: 2 }` |
| main | `fn main()`, `fn main() -> i32`, `fn main(argc: i32, argv: **u8) -> i32` |
| Compile time | `const`, `when`, generic functions and structs (`fn f[T]`, `struct S[T]`) — see below |

Operator precedence is similar to C, but bitwise operators (`& | ^`) bind more tightly than comparison operators (as in Go and Rust).

## Compile-Time Programming

Hoshi has no separate macro language. Compile-time features are ordinary Hoshi code that the compiler evaluates or specializes, so they cost nothing at run time.

### Constants

```hoshi
const PAGE = 4 * 1024;      // untyped: adapts to the context, like a literal
const MASK: u8 = 0xff;      // typed
const NAME = "hoshi";

let a: u64 = PAGE;          // PAGE is a u64 here...
let b: i32 = PAGE;          // ...and an i32 here
var buf: [PAGE * 2]u8;      // array lengths are constant expressions
```

Constant expressions support arithmetic, bitwise, comparison and logical operators, `as` casts, and other constants (in any declaration order). Overflow and division by zero are compile errors.

### Conditional compilation with `when`

```hoshi
when OS == "linux" {
    extern fn epoll_create1(flags: i32) -> i32;
} else when OS == "macos" {
    extern fn kqueue() -> i32;
}

const DEBUG = false;        // default; `hoshic -D DEBUG` turns it on

fn main() {
    when DEBUG {
        printf("debug build\n");
    }
}
```

`when` works at the top level (choosing declarations) and inside functions. Only the chosen branch is type checked, so inactive branches may refer to platform-specific names. Predefined constants: `OS` (`"linux"`, `"macos"`, `"windows"`, ...), `ARCH` (`"x86_64"`, `"aarch64"`, ...), and `OPT_LEVEL`. `-D NAME[=VALUE]` on the command line replaces the value of a global constant with that name, so declared constants act as defaults; if no such constant exists, it defines a new one. `-D DEBUG` means `true`; numbers and strings are parsed as such.

### Generics

```hoshi
fn max[T](a: T, b: T) -> T {
    if a > b { return a; }
    return b;
}

struct Vec[T] { data: *T, len: u64, cap: u64 }

fn alloc[T](n: u64) -> *T {
    return malloc(n * sizeof(T)) as *T;
}

let m = max(x, 1);             // T inferred from the arguments
let v = Vec[i64] { data: null };
let p = alloc[Node[i32]](1);   // explicit type arguments when inference is impossible
```

Generics are monomorphized: every combination of type arguments gets its own specialized copy, exactly as if written by hand, so they are as fast as non-generic code. Generic bodies are type checked per instantiation (like Zig and C++ templates); errors point at both the problem and the call that requested the instance.

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
  -D NAME[=VALUE]  Define a compile-time constant
  -l<lib> -L<dir>  Pass options to the linker
```

Executables are linked with a system C compiler driver (tried in this order: `clang` → `cc` → `gcc`; configurable via `$HOSHI_CC`).

## Project Structure

```
src/lexer.*      Tokenization
src/parser.*     Recursive descent + precedence-based expression parsing → AST (src/ast.h)
src/sema.*       Name resolution, type checking, constant evaluation, `when`, generic instantiation
src/ast_clone.cpp AST copies for generic instantiation
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
- Function pointers
- Compile-time function execution (`comptime`) and reflection over struct fields
- Interfaces/constraints for generics, type comparisons in `when` (e.g. `when T == f32`)
