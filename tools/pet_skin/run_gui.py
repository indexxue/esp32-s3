#!/usr/bin/env python3
"""Launch pet_skin Qt GUI (PySide6)."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from app.main import main

if __name__ == "__main__":
    raise SystemExit(main())
