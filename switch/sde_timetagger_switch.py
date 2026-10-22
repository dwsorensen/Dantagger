"""`import TimeTagger` -> Dantagger for the SDE venv (installed 2026-10-21).

Loaded at interpreter start by `dantagger_timetagger_hook.pth` in the venv's
site-packages (written by Dantagger/tools/install_timetagger_hook.py).  It
only adds an import finder: nothing is imported until a script imports
TimeTagger, which then gets the Dantagger module.

    python script.py                        # Dantagger (default)
    SDE_TAGGER=swabian python script.py     # the Swabian library for this run

This directory holds only this module, because the .pth puts it on sys.path.
"""

import importlib
import importlib.machinery
import os
import sys

# The Dantagger package lives in <repo>/python (this file is <repo>/switch/...).
PYDIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "python")
ENV = "SDE_TAGGER"


def enabled():
    return os.environ.get(ENV, "dantagger").strip().lower() != "swabian"


class _Loader:   # duck-typed: importlib.abc costs ~35 ms at every interpreter start
    def create_module(self, spec):
        if PYDIR not in sys.path:
            sys.path.append(PYDIR)
        import Dantagger
        return Dantagger

    def exec_module(self, module):
        pass


class _Finder:
    def find_spec(self, name, path=None, target=None):
        if name != "TimeTagger" or not enabled():
            return None
        return importlib.machinery.ModuleSpec("TimeTagger", _Loader())


def _is_finder(f):
    # Also a finder of another copy of this switch (e.g. SDE/Dantagger's).
    return type(f).__name__ == "_Finder" and type(f).__module__ == __name__


def install():
    if not any(_is_finder(f) for f in sys.meta_path):
        sys.meta_path.insert(0, _Finder())


def import_swabian():
    """The real Swabian module, even while TimeTagger means Dantagger (oracle tests)."""
    if "_swabian_TimeTagger" in sys.modules:
        return sys.modules["_swabian_TimeTagger"]
    saved = sys.modules.pop("TimeTagger", None)
    finders = [f for f in sys.meta_path if _is_finder(f)]
    for f in finders:
        sys.meta_path.remove(f)
    try:
        mod = importlib.import_module("TimeTagger")
    finally:
        sys.modules.pop("TimeTagger", None)
        if saved is not None:
            sys.modules["TimeTagger"] = saved
        for f in finders:
            sys.meta_path.insert(0, f)
    sys.modules["_swabian_TimeTagger"] = mod
    return mod


install()
