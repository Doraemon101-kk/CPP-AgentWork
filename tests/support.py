"""Shared discovery for source-tree and CTest runs."""
import os
from pathlib import Path
APP = Path(__file__).resolve().parents[1]
EXE = Path(os.environ.get("CPPPILOT_EXE", str(APP / "build/bin/cpp_agent.exe"))).resolve()
if not EXE.is_file():
    raise RuntimeError("Build first, or set CPPPILOT_EXE to the built cpp_agent.exe")
SCRATCH = EXE.parent.parent / "test-work"
SCRATCH.mkdir(parents=True, exist_ok=True)
