"""Compatibility entry point for host/windows/desktop.py."""
from pathlib import Path
import sys

if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host" / "windows"))
    from desktop import main
    main()
