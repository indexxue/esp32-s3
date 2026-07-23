#!/usr/bin/env python3
"""Extract embedded HTML from web_server.c into a standalone file."""

import os
import re
import sys


def unesc(s: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            c = s[i + 1]
            if c == "n":
                out.append("\n")
                i += 2
            elif c == "t":
                out.append("\t")
                i += 2
            elif c == "r":
                out.append("\r")
                i += 2
            elif c == "u" and i + 6 <= len(s):
                out.append(chr(int(s[i + 2 : i + 6], 16)))
                i += 6
            else:
                out.append(c)
                i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else "components/web_ctrl/src/web_server.c"
    dst = sys.argv[2] if len(sys.argv) > 2 else "project/main/source/www/index.html"

    text = open(src, encoding="utf-8").read()
    m = re.search(
        r"static const char html\[\] =\s*(.*?);\s*\n\s*\(void\)httpd_resp_set_type",
        text,
        re.DOTALL,
    )
    if not m:
        print("HTML block not found", file=sys.stderr)
        return 1

    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    html = unesc("".join(parts))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    print(f"written {len(html)} chars -> {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
