# hoshi

배우기 쉬운 단순한 문법에, C와 같은 성능과 저수준 제어를 목표로 하는 시스템 프로그래밍 언어입니다.
컴파일러 `hoshic`은 C++20과 LLVM으로 작성되었습니다.

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

## 설계 원칙

- **C와 같은 의미론**: 값 타입, 포인터, 수동 메모리 관리. 런타임·GC·예외가 없고 숨겨진 할당도 없습니다.
- **C ABI 호환**: `extern fn`으로 libc 등 C 함수를 바로 호출합니다. 이름 맹글링이 없습니다.
- **암시적 변환 없음**: 타입 변환은 항상 `as`로 명시합니다. 정수 리터럴만 문맥에 맞춰 타입이 정해집니다.
- **최적화는 LLVM에**: 프론트엔드는 단순하게 두고 clang과 같은 `-O2`/`-O3` 파이프라인을 씁니다.
  부호 있는 정수 오버플로는 C처럼 정의되지 않은 동작입니다.

## 언어 요약 (v0)

| 분류 | 내용 |
|---|---|
| 타입 | `i8 i16 i32 i64 u8 u16 u32 u64 isize usize f32 f64 bool void`, `*T`, `[N]T`, `struct` |
| 변수 | `let`(불변), `var`(가변, 초기값 없으면 0으로 초기화), 타입 추론 |
| 제어 | `if`/`else if`/`else`, `while`, `for i in a..b`(b 미포함), `break`, `continue`, `return` |
| 정리 | `defer stmt;` / `defer { ... }` — 스코프를 벗어날 때 역순 실행 (`return`/`break` 포함) |
| 식 | 산술·비트·비교·논리(단축 평가), `&x`, `*p`, `p[i]`, `p + n`, `p - q`, `s.f`(포인터면 자동 역참조), `as`, `sizeof(T)` |
| 리터럴 | `42`, `0xff`, `0b1010`, `1_000`, `3.14`, `'a'`(u8), `"str"`(`*u8`, null 종료), `true`, `false`, `null`, `[1, 2, 3]`, `Point { x: 1, y: 2 }` |
| main | `fn main()`, `fn main() -> i32`, `fn main(argc: i32, argv: **u8) -> i32` |

연산자 우선순위는 C와 비슷하지만, 비트 연산자(`& | ^`)가 비교 연산자보다 먼저 묶입니다(Go/Rust와 동일).

## 개발 환경

[Nix flake](https://nixos.wiki/wiki/Flakes)로 LLVM 21, clang, CMake, Ninja, clangd 등이 갖춰진 환경을 띄웁니다.

```sh
nix develop          # 또는 direnv: `direnv allow` (.envrc 포함)
cmake -B build -G Ninja
ninja -C build

./build/hoshic examples/hello.hoshi -o hello && ./hello
python3 tests/run.py build/hoshic   # 골든 테스트 (-O0, -O2 모두 실행)
bench/compare.sh                   # 같은 프로그램의 C 버전(clang -O2)과 속도 비교
```

패키지 빌드(테스트 포함): `nix build`, `nix flake check`. 결과물 `result/bin/hoshic`은 링크용 clang을 자동으로 PATH에 넣습니다.

## hoshic 사용법

```
hoshic [options] <file.hoshi>
  -o <path>        출력 경로
  -O0 .. -O3       최적화 수준 (기본 -O0)
  -c / -S          오브젝트 파일 / 어셈블리 출력
  --emit-llvm      LLVM IR 출력
  --dump-ast       파싱된 AST 출력
  --march=native   현재 CPU에 맞춰 최적화
  -l<lib> -L<dir>  링커로 전달
```

실행 파일은 시스템 C 컴파일러 드라이버(`clang` → `cc` → `gcc` 순, `$HOSHI_CC`로 지정 가능)로 링크합니다.

## 구조

```
src/lexer.*      토큰화
src/parser.*     재귀 하강 + 우선순위 기반 식 파싱 → AST (src/ast.h)
src/sema.*       이름 해석, 타입 검사, 가변성/반환 경로 검사
src/codegen.*    AST → LLVM IR (지역 변수는 alloca → mem2reg로 SSA화)
src/driver.*     최적화 파이프라인, 오브젝트 생성, 링크
tests/cases      실행 결과 골든 테스트 (// expect:, // exit:)
tests/errors     컴파일 에러 테스트 (// error:)
examples/        hello, fib, nbody
bench/           C 비교 벤치마크
```

## 로드맵

- 모듈/import, enum과 tagged union, slice(`[]T`), 전역 변수
- 디버그 정보(DWARF), 여러 에러를 한 번에 보고
- 함수 포인터, 제네릭 검토
