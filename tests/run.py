#!/usr/bin/env python3
"""Golden tests for hoshic.

tests/cases/*.hoshi   compiled at -O0 and -O2, run, and checked against
                      `// expect: <line>` comments (stdout, one per line)
                      and an optional `// exit: <code>` comment.
tests/errors/*.hoshi  must fail to compile with a message containing the
                      text of each `// error: <text>` comment.

usage: run.py <path-to-hoshic> [filter]
"""

import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent


def directives(path, key):
    prefix = f"// {key}:"
    out = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line.startswith(prefix):
            out.append(line[len(prefix):].removeprefix(" "))
    return out


def run_case(hoshic, path, tmp):
    expect = directives(path, "expect")
    exit_lines = directives(path, "exit")
    want_exit = int(exit_lines[0]) if exit_lines else 0
    want_out = "".join(line + "\n" for line in expect)

    for opt in ("-O0", "-O2"):
        exe = pathlib.Path(tmp) / f"{path.stem}{opt}"
        c = subprocess.run([hoshic, opt, str(path), "-o", str(exe)],
                           capture_output=True, text=True)
        if c.returncode != 0:
            return f"[{opt}] compile failed:\n{c.stderr}"
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        if r.stdout != want_out:
            return f"[{opt}] stdout mismatch\n--- expected\n{want_out}--- got\n{r.stdout}"
        if r.returncode != want_exit:
            return f"[{opt}] exit code {r.returncode}, expected {want_exit}"
    return None


def run_error(hoshic, path, tmp):
    wants = directives(path, "error")
    if not wants:
        return "missing // error: directive"
    c = subprocess.run([hoshic, str(path), "-o", str(pathlib.Path(tmp) / path.stem)],
                       capture_output=True, text=True)
    if c.returncode == 0:
        return "compiled successfully but an error was expected"
    for want in wants:
        if want not in c.stderr:
            return f"expected error containing {want!r}, got:\n{c.stderr}"
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    hoshic = str(pathlib.Path(sys.argv[1]).resolve())
    pattern = sys.argv[2] if len(sys.argv) > 2 else ""

    tests = [(p, run_case) for p in sorted((ROOT / "cases").glob("*.hoshi"))]
    tests += [(p, run_error) for p in sorted((ROOT / "errors").glob("*.hoshi"))]
    tests = [(p, fn) for p, fn in tests if pattern in p.name]

    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path, fn in tests:
            name = path.relative_to(ROOT)
            err = fn(hoshic, path, tmp)
            if err:
                failed += 1
                print(f"FAIL {name}\n{err}")
            else:
                print(f"ok   {name}")

    print(f"\n{len(tests) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
