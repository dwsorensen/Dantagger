"""Test setup: the package from <repo>/python, the switch from <repo>/switch,
and the SDE tree (read only: its scripts and datautilities) from
$DANTAGGER_SDE (default /home/qittlab/dws/SDE)."""

import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SDE = pathlib.Path(os.environ.get("DANTAGGER_SDE", "/home/qittlab/dws/SDE"))
for p in (ROOT / "python", ROOT / "switch"):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))
