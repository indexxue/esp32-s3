"""Qt application entry for pet_skin."""

from __future__ import annotations

import sys
from pathlib import Path


def main() -> int:
    tool_root = Path(__file__).resolve().parents[1]
    if str(tool_root) not in sys.path:
        sys.path.insert(0, str(tool_root))

    try:
        from PySide6.QtWidgets import QApplication
    except ImportError:
        print(
            "PySide6 required. Install:\n"
            "  py -3 -m pip install -r tools/pet_tool/skin/requirements.txt",
            file=sys.stderr,
        )
        return 2

    from app.main_window import MainWindow

    qapp = QApplication(sys.argv)
    qapp.setApplicationName("pet_skin")
    qapp.setOrganizationName("desktop_pet")
    win = MainWindow()
    win.show()
    return qapp.exec()


if __name__ == "__main__":
    raise SystemExit(main())
