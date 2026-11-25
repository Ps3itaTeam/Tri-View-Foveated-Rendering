"""Takes a shader's text out of a C++ source of the mod: the raw string literals of `constexpr char NAME[] = R"(...)" ...;`
joined, as the compiler joins them.
   extract_shader.py <file.cpp> <NAME> [<out file>]
Prints the size of every literal and of the whole (MSVC takes at most 16380 bytes in one literal: longer shaders are
written as several) and writes the text when an output file is given (for gpu/periphery_test.exe: the eye pass's
shader, or `compile` of any entry point with the real D3DCompile)."""
import re
import sys


def literals(src, name):
    m = re.search(r"constexpr char " + re.escape(name) + r"\[\]\s*=", src)
    if m is None:
        raise SystemExit(f"{name}: not found")
    parts = []
    pos = m.end()
    while True:
        nxt = re.compile(r'R"\(').search(src, pos)
        if nxt is None:
            break
        between = re.sub(r"//[^\n]*", "", src[pos:nxt.start()])
        if ";" in between:  # the constant ended before the next literal
            break
        close = src.index(')"', nxt.end())
        parts.append(src[nxt.end():close])
        pos = close + 2
    return parts


if __name__ == "__main__":
    text = open(sys.argv[1], encoding="utf-8").read()
    parts = literals(text, sys.argv[2])
    sizes = [len(p.encode("utf-8")) for p in parts]
    print(f"{sys.argv[2]}: {len(parts)} literals of {sizes} bytes (the longest {max(sizes)}, at most 16380), {sum(sizes)} in all")
    if len(sys.argv) > 3:
        open(sys.argv[3], "w", encoding="utf-8", newline="\n").write("".join(parts))
    sys.exit(0 if max(sizes) <= 16380 else 1)
