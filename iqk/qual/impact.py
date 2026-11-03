"""Change impact: which requirements a change puts back to "to be re-verified", and why.

A requirement is affected when the change touches an interface document or configuration value it cites
(config values are compared by key, so editing an unrelated setting does not re-open it), the code its
mutants live in, or a test that claims it.
"""

from __future__ import annotations

import json
import re
import subprocess
from collections import defaultdict

import yaml

from ..icd import ROOT
from . import mutate
from .matrix import REPORTS, requirements


def _git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def _yaml_at(ref: str, path: str) -> dict:
    try:
        return yaml.safe_load(_git("show", f"{ref}:{path}")) or {}
    except subprocess.CalledProcessError:
        return {}


def _get(d: dict, dotted: str):
    for part in dotted.split("."):
        if not isinstance(d, dict) or part not in d:
            return None
        d = d[part]
    return d


def impact(base: str, head: str = "HEAD") -> dict:
    changed = [f for f in _git("diff", "--name-only", f"{base}..{head}").splitlines() if f]
    tests = (
        json.loads((REPORTS / "qualification" / "tests.json").read_text())
        if (REPORTS / "qualification" / "tests.json").exists()
        else []
    )
    gtest_file = {}  # Suite.Name -> the C++ file that defines it
    for f in sorted((ROOT / "cpp" / "tests").glob("*.cpp")):
        for suite, name in re.findall(r"^TEST\((\w+),\s*(\w+)\)", f.read_text(), flags=re.M):
            gtest_file[f"{suite}.{name}"] = str(f.relative_to(ROOT))
    test_files = defaultdict(set)  # requirement -> files of tests claiming it
    for t in tests:
        kind, _, rest = t["id"].partition(":")
        path = rest.split("::")[0] if kind == "pytest" else gtest_file.get(rest, "")
        for r in t["verifies"]:
            if path:
                test_files[r].add(path)
    code_files = defaultdict(set)
    for m in mutate.load():
        code_files[m.requirement].add(m.file)
    old_cfg, new_cfg = {}, {}
    affected = []
    for r in requirements():
        reasons = []
        for c in r.get("cites", []):
            path, _, key = c.partition("#")
            if path not in changed:
                continue
            if key:
                if not old_cfg.get(path):
                    old_cfg[path] = _yaml_at(base, path)
                    new_cfg[path] = _yaml_at(head, path)
                if _get(old_cfg[path], key) != _get(new_cfg[path], key):
                    reasons.append(f"{key} changed in {path}")
            else:
                reasons.append(f"{path} changed")
        for f in sorted(code_files[r["id"]] & set(changed)):
            reasons.append(f"code {f} changed")
        for f in sorted(test_files[r["id"]] & set(changed)):
            reasons.append(f"test {f} changed")
        if reasons:
            affected.append({"id": r["id"], "title": r["title"], "reasons": reasons})
    return {"base": base, "head": head, "changed_files": changed, "requirements_to_reverify": affected}
