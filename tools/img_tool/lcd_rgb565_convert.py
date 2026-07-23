#!/usr/bin/env python3
"""转发到 `project/tools/lcd_rgb565_convert.py`（实现已迁至该文件）。"""

from __future__ import annotations

import runpy
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
_MAIN = _REPO / "project" / "tools" / "lcd_rgb565_convert.py"

if not _MAIN.is_file():
    print(f"Missing {_MAIN}", file=sys.stderr)
    raise SystemExit(2)

sys.argv[0] = str(_MAIN)
runpy.run_path(str(_MAIN), run_name="__main__")
