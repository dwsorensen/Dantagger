"""Install (or remove) the switch that makes `import TimeTagger` load Dantagger.

    .venv/bin/python Dantagger/tools/install_timetagger_hook.py             # install
    .venv/bin/python Dantagger/tools/install_timetagger_hook.py --uninstall # back to Swabian

Writes ``dantagger_timetagger_hook.pth`` into the venv's site-packages.  It
puts Dantagger/switch on sys.path and imports sde_timetagger_switch, which
adds a lazy import finder: every `import TimeTagger` in this venv gets
Dantagger, unless SDE_TAGGER=swabian is set for that run.  The Swabian
package itself is not touched.  Installed 2026-10-21 at the user's request.
"""

import argparse
import pathlib
import sysconfig

NAME = "dantagger_timetagger_hook.pth"
SWITCH_DIR = pathlib.Path(__file__).resolve().parents[1] / "switch"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uninstall", action="store_true")
    args = ap.parse_args()
    target = pathlib.Path(sysconfig.get_paths()["purelib"]) / NAME
    if args.uninstall:
        if target.exists():
            target.unlink()
            print(f"removed {target}: `import TimeTagger` is the Swabian library again")
        else:
            print("not installed")
        return
    target.write_text(f"{SWITCH_DIR}\nimport sde_timetagger_switch\n")
    print(f"wrote {target}\n`import TimeTagger` now gives Dantagger; SDE_TAGGER=swabian selects Swabian for one run.")


if __name__ == "__main__":
    main()
