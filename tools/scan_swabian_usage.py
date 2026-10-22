"""List the Swabian TimeTagger API that each script in the SDE tree uses.

Static analysis only (Python AST, nothing is imported or run).  For every file
it records:

* module-level names taken from ``TimeTagger`` (``TimeTagger.X`` or
  ``from TimeTagger import X``);
* method calls on objects that came from those names, attributed to the class
  that made the object (``Countrate.getData``, ``TimeTagger.setTriggerLevel``,
  ``SynchronizedMeasurements.getTagger`` ...), with the keyword arguments used;
* use of the ``datautilities`` wrapper classes (``ttCountRate`` ...), which
  are expanded into the API they call.

Usage::

    .venv/bin/python Dantagger/tools/scan_swabian_usage.py [--markdown OUT.md] [ROOT]
"""

import argparse
import ast
import collections
import pathlib
import re
import sys
import warnings

warnings.filterwarnings("ignore", category=SyntaxWarning)   # old escape sequences in scanned files

# Class of the object returned by a TimeTagger factory / method.
RETURNS = {
    "createTimeTagger": "TimeTagger",
    "SynchronizedMeasurements.getTagger": "TimeTagger(proxy)",
    "TimeTagStream.getData": "TimeTagStreamBuffer",
}
# Receivers that are tagger objects passed in as parameters.
TAGGER_NAME = re.compile(r"(^|_)tagger$|^tt$", re.I)

# What each datautilities wrapper calls (read from datautilities.py).
WRAPPERS = {
    "ttCountRate": [
        "createTimeTagger", "TimeTagger.setTriggerLevel", "TimeTagger.setDeadtime",
        "SynchronizedMeasurements", "SynchronizedMeasurements.getTagger", "Countrate",
        "SynchronizedMeasurements.startFor", "SynchronizedMeasurements.waitUntilFinished",
        "Countrate.getData",
    ],
    "ttCountRateHist": [
        "createTimeTagger", "TimeTagger.setTriggerLevel", "TimeTagger.setDeadtime",
        "TimeTagger.setInputDelay", "SynchronizedMeasurements", "SynchronizedMeasurements.getTagger",
        "Countrate", "Histogram", "SynchronizedMeasurements.startFor",
        "SynchronizedMeasurements.waitUntilFinished", "Countrate.getData", "Histogram.getData",
    ],
    "ttHist1D": [
        "createTimeTagger", "TimeTagger.setTriggerLevel", "TimeTagger.setDeadtime",
        "TimeTagger.setInputDelay", "SynchronizedMeasurements", "SynchronizedMeasurements.getTagger",
        "Histogram", "SynchronizedMeasurements.startFor", "SynchronizedMeasurements.waitUntilFinished",
        "Histogram.getData", "Histogram.getIndex",
    ],
    "ttHist": [
        "createTimeTagger", "TimeTagger.setTriggerLevel", "TimeTagger.setDeadtime",
        "TimeTagger.setInputDelay", "SynchronizedMeasurements", "SynchronizedMeasurements.getTagger",
        "Correlation", "SynchronizedMeasurements.startFor", "SynchronizedMeasurements.waitUntilFinished",
        "Correlation.getData", "Correlation.getIndex",
    ],
}
WRAPPER_ATTRS = {  # attribute of a wrapper instance -> class it holds
    "tagger": "TimeTagger", "synchronized": "SynchronizedMeasurements",
    "syncProxy": "TimeTagger(proxy)", "countRate": "Countrate",
    "histogram": "Histogram", "correlation": "Correlation",
}
# Swabian methods are camelCase; this drops look-alike helpers (set_trigger ...)
# that the receiver-name heuristic would otherwise attribute to a tagger class.
SWABIAN_METHOD = re.compile(r"[a-z]+(?:[A-Z0-9][a-z0-9]*)*[A-Za-z0-9]*")
SKIP_DIRS = {".venv", "__pycache__", "Dantagger", ".git", "site-packages"}


def unparse(node):
    try:
        return ast.unparse(node)
    except Exception:
        return "?"


def scan_file(path):
    tree = ast.parse(path.read_text(encoding="utf-8", errors="replace"))
    modules, names, wrapper_names = set(), {}, {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for a in node.names:
                if a.name == "TimeTagger":
                    modules.add(a.asname or a.name)
        elif isinstance(node, ast.ImportFrom):
            for a in node.names:
                if node.module == "TimeTagger":
                    names[a.asname or a.name] = a.name
                elif node.module == "datautilities" and a.name == "*":
                    wrapper_names.update({w: w for w in WRAPPERS})
                elif node.module == "datautilities" and a.name in WRAPPERS:
                    wrapper_names[a.asname or a.name] = a.name
    du_modules = {a.asname or a.name for n in ast.walk(tree) if isinstance(n, ast.Import)
                  for a in n.names if a.name == "datautilities"}

    used = collections.defaultdict(set)      # api name -> keyword args seen
    lines = collections.defaultdict(set)
    wrappers = collections.defaultdict(set)  # wrapper -> methods called on it
    kinds = {}                               # unparsed receiver -> class / wrapper

    def symbol(func):
        if isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name) and func.value.id in modules:
            return func.attr
        if isinstance(func, ast.Name) and func.id in names:
            return names[func.id]
        return None

    def wrapper_of(func):
        if isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name) \
                and func.value.id in du_modules and func.attr in WRAPPERS:
            return func.attr
        if isinstance(func, ast.Name) and func.id in wrapper_names:
            return wrapper_names[func.id]
        return None

    def kind_of_expr(expr):
        """Class of the object an expression evaluates to, if known."""
        text = unparse(expr)
        if text in kinds:
            return kinds[text]
        if isinstance(expr, ast.Attribute):
            owner = kind_of_expr(expr.value)
            if owner and owner.startswith("wrapper:") and expr.attr in WRAPPER_ATTRS:
                return WRAPPER_ATTRS[expr.attr]
        if isinstance(expr, ast.Call):
            sym = symbol(expr.func)
            if sym:
                return RETURNS.get(sym, sym)
            w = wrapper_of(expr.func)
            if w:
                return "wrapper:" + w
            if isinstance(expr.func, ast.Attribute):
                owner = kind_of_expr(expr.func.value)
                if owner:
                    return RETURNS.get(f"{owner}.{expr.func.attr}")
        last = text.split(".")[-1]
        if TAGGER_NAME.search(last):
            return "TimeTagger"
        return None

    # Resolve assignments to a fixed point (objects built from other objects).
    for _ in range(4):
        for node in ast.walk(tree):
            if isinstance(node, (ast.Assign, ast.AnnAssign)) and node.value is not None:
                k = kind_of_expr(node.value)
                if k:
                    for t in (node.targets if isinstance(node, ast.Assign) else [node.target]):
                        kinds[unparse(t)] = k

    for node in ast.walk(tree):
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) and node.value.id in modules:
            used[node.attr]
            lines[node.attr].add(node.lineno)
        elif isinstance(node, ast.Name) and node.id in names and isinstance(node.ctx, ast.Load):
            used[names[node.id]]
            lines[names[node.id]].add(node.lineno)
        if not isinstance(node, ast.Call):
            continue
        kw = {k.arg for k in node.keywords if k.arg}
        sym = symbol(node.func)
        if sym:
            used[sym] |= kw
            continue
        w = wrapper_of(node.func)
        if w:
            wrappers[w].add("__init__")
            continue
        if isinstance(node.func, ast.Attribute):
            owner = kind_of_expr(node.func.value)
            if owner and owner.startswith("wrapper:"):
                wrappers[owner[8:]].add(node.func.attr)
            elif owner and SWABIAN_METHOD.fullmatch(node.func.attr):
                api = f"{owner.replace('(proxy)', '')}.{node.func.attr}"
                used[api] |= kw
                lines[api].add(node.lineno)
    imported_only = sorted(set(names.values()) - set(used))
    return used, lines, wrappers, imported_only


def iter_files(root):
    for path in sorted(root.rglob("*.py")):
        if path.name.startswith("._") or any(part in SKIP_DIRS for part in path.relative_to(root).parts):
            continue   # ._* are macOS resource forks
        yield path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("root", nargs="?", default=".")
    ap.add_argument("--markdown", help="write the per-script table to this file")
    args = ap.parse_args()
    root = pathlib.Path(args.root).resolve()

    per_file = {}
    totals = collections.defaultdict(set)
    import_only = collections.defaultdict(set)   # imported, never used: must exist for the import
    for path in iter_files(root):
        try:
            used, _lines, wrappers, imported_only = scan_file(path)
        except (SyntaxError, ValueError) as exc:
            print(f"skip {path}: {exc}", file=sys.stderr)
            continue
        api = dict(used)
        for w, methods in wrappers.items():
            for name in WRAPPERS[w]:
                api.setdefault(name, set())
            if "setInput" in methods:
                api.setdefault("TimeTagger.setTriggerLevel", set())
                api.setdefault("TimeTagger.setDeadtime", set())
        api = {k: v for k, v in api.items() if k != "TimeTagger"}
        if not api and not wrappers:
            continue
        rel = str(path.relative_to(root))
        per_file[rel] = (api, wrappers, imported_only)
        for name in api:
            totals[name].add(rel)
        for name in imported_only:
            import_only[name].add(rel)

    out = []
    out.append("| API | Scripts |\n|---|---|")
    for name in sorted(totals, key=lambda n: (-len(totals[n]), n)):
        out.append(f"| `{name}` | {len(totals[name])} |")
    out.append("")
    out.append("Imported but never used (must exist so the import succeeds):\n")
    out.append("| Name | Scripts |\n|---|---|")
    for name in sorted(import_only):
        out.append(f"| `{name}` | {len(import_only[name])} |")
    out.append("")
    out.append("| Script | Wrappers (datautilities) | TimeTagger API (keyword args) | Imported only |\n|---|---|---|---|")
    for rel, (api, wrappers, imported_only) in per_file.items():
        w = ", ".join(f"{k}({', '.join(sorted(m - {'__init__'}))})" for k, m in sorted(wrappers.items())) or "–"
        a = ", ".join(f"`{k}`" + (f" [{', '.join(sorted(v))}]" if v else "") for k, v in sorted(api.items()))
        i = ", ".join(f"`{n}`" for n in imported_only) or "–"
        out.append(f"| `{rel}` | {w} | {a} | {i} |")
    text = "\n".join(out) + "\n"
    if args.markdown:
        pathlib.Path(args.markdown).write_text(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
