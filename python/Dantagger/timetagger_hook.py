"""Make ``import TimeTagger`` load Dantagger inside one process.

Since 2026-10-21 the SDE venv does this for every run already (the switch in
Dantagger/switch, installed by tools/install_timetagger_hook.py; opt out per
run with SDE_TAGGER=swabian).  activate() is for interpreters without that
switch, e.g. another venv:

    import Dantagger.timetagger_hook; Dantagger.timetagger_hook.activate()
    import TimeTagger          # -> Dantagger
"""

import sys


def activate():
    """Alias the TimeTagger module name to Dantagger (call before `import TimeTagger`)."""
    current = sys.modules.get("TimeTagger")
    if current is not None and getattr(current, "__name__", "") != "Dantagger":
        raise RuntimeError("the Swabian TimeTagger module is already imported; activate the hook earlier")
    import Dantagger
    sys.modules["TimeTagger"] = Dantagger
    return Dantagger
