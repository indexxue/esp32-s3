#!/usr/bin/env python3
"""Build ballot_guard index.html with Wi-Fi provisioning UI from project index.html."""

import re
import sys


def extract_between(html: str, start: str, end: str) -> str:
    i = html.find(start)
    if i < 0:
        raise ValueError(f"start marker not found: {start!r}")
    j = html.find(end, i + len(start))
    if j < 0:
        raise ValueError(f"end marker not found: {end!r}")
    return html[i:j + len(end)]


def extract_functions(script: str, names: list[str]) -> str:
    hits: list[tuple[int, str]] = []
    for m in re.finditer(r"(async\s+)?function\s+([a-zA-Z0-9_]+)", script):
        prefix = m.group(1) or ""
        fname = m.group(2)
        hits.append((m.start(), f"{prefix}function {fname}"))

    pos = {name.split()[-1]: start for start, name in hits}
    ordered = [name.split()[-1] for _, name in hits]
    out: list[str] = []
    for name in names:
        short = name.split()[-1]
        if short not in pos:
            raise ValueError(f"missing function: {name}")
        start = pos[short]
        idx = ordered.index(short)
        end = pos[ordered[idx + 1]] if idx + 1 < len(ordered) else len(script)
        chunk = script[start:end].rstrip()
        if chunk.endswith("async"):
            chunk = chunk[:-5].rstrip()
        out.append(chunk)
    return "".join(out)


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else "project/main/source/www/index.html"
    dst = sys.argv[2] if len(sys.argv) > 2 else "ballot_guard/main/source/www/index.html"

    html = open(src, encoding="utf-8").read()
    script = re.search(r"<script>(.*)</script>", html, re.DOTALL).group(1)

    style = extract_between(html, "<style>", "</style>")
    card_wifi = extract_between(html, "<section class='card' id='card-wifi'>", "</section>")
    modal = extract_between(
        html,
        "<div class='modal-wrap' id='modal-disconnect'",
        "</div></div>",
    )

    wifi_names = [
        "esc",
        "setBusy",
        "showLog",
        "rssiPct",
        "refreshStatus",
        "doScan",
        "doSave",
        "openDisconnectModal",
        "closeDisconnectModal",
        "confirmStaDisconnect",
    ]
    js = extract_functions(script, wifi_names)
    js = js.replace(
        "['btn-refresh','btn-scan','btn-save','btn-run','btn-up-bmp','btn-gal-refresh','btn-gal-clear-boot','btn-disconnect','modal-confirm-dc','btn-vid-refresh','btn-vid-stop','btn-up-vid']",
        "['btn-refresh','btn-scan','btn-save','btn-disconnect','modal-confirm-dc']",
    )
    js = js.replace(
        "document.querySelectorAll('.fx-busy').forEach(function(el){el.disabled=b;});",
        "",
    )
    tail = (
        "document.getElementById('modal-disconnect').addEventListener('click',function(ev){"
        "if(ev.target.id==='modal-disconnect')closeDisconnectModal();});"
        "refreshStatus();setInterval(refreshStatus,2500);"
    )

    out = f"""<!DOCTYPE html>
<html lang='zh-CN'>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Ballot Guard</title>
{style}
</head>
<body>
<div class='wrap'>
<header class='top'>
<h1>Ballot Guard</h1>
<p>选票监管 · SoftAP 配网与 STA 连接（策略 A：先 STA，失败回退 SoftAP）</p>
</header>
{card_wifi}
{modal}
</div>
<script>
{js}{tail}
</script>
</body>
</html>
"""

    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)
    print(f"written {len(out)} chars -> {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
