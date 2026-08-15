#!/usr/bin/env python3
"""Static server for pet_tool: skin assets + web preview on one origin."""

from __future__ import annotations

import argparse
import functools
import sys
import webbrowser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEFAULT_PORT = 8765


class PetToolHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def do_GET(self):  # noqa: N802
        if self.path in ("/", "/index.html"):
            self.send_response(302)
            self.send_header("Location", "/web/")
            self.end_headers()
            return
        if self.path == "/web":
            self.send_response(302)
            self.send_header("Location", "/web/")
            self.end_headers()
            return
        if self.path == "/web/":
            self.path = "/web/index.html"
        return super().do_GET()

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main() -> int:
    ap = argparse.ArgumentParser(description="Serve pet_tool (skin + web preview)")
    ap.add_argument("--port", "-p", type=int, default=DEFAULT_PORT)
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--bind", default="127.0.0.1")
    args = ap.parse_args()

    handler = functools.partial(PetToolHandler)
    httpd = ThreadingHTTPServer((args.bind, args.port), handler)
    url = f"http://{args.bind}:{args.port}/web/"
    print(f"pet_tool root: {ROOT}")
    print(f"preview:       {url}")
    print(f"skin pack:     http://{args.bind}:{args.port}/skin/pack.json")
    print("Ctrl+C to stop.")
    if not args.no_browser:
        try:
            webbrowser.open(url)
        except Exception:
            pass
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped.")
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
