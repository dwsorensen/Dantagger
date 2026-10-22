"""Every Swabian name and method the SDE scripts use exists in Dantagger.

Runs tools/scan_swabian_usage.py over the SDE tree (drivers/amcc is out of
scope, user 2026-10-21) and checks each API name against the Dantagger module.
"""

import importlib.util
import pathlib

import Dantagger

from tests.conftest import ROOT, SDE


def scanner():
    spec = importlib.util.spec_from_file_location("scan", ROOT / "tools" / "scan_swabian_usage.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def used_names():
    scan = scanner()
    names = set()
    for path in scan.iter_files(SDE):
        rel = path.relative_to(SDE).as_posix()
        if rel.startswith("drivers/amcc/"):
            continue
        try:
            used, _lines, wrappers, imported_only = scan.scan_file(path)
        except (SyntaxError, ValueError):
            continue
        names |= set(used) | set(imported_only)
        for w in wrappers:
            names |= set(scan.WRAPPERS[w])
    return names


def test_every_used_name_exists():
    missing = []
    for name in sorted(used_names()):
        if "." in name:
            cls, meth = name.split(".", 1)
            owner = getattr(Dantagger, "TimeTagger" if cls == "TimeTagger" else cls, None)
            if owner is None or not hasattr(owner, meth):
                missing.append(name)
        elif not hasattr(Dantagger, name):
            missing.append(name)
    assert not missing, missing
    assert len(used_names()) > 30
