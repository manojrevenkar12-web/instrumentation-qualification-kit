"""Held-out conformance sets: written from the documents alone by someone who never read the code, run once.

The first run is written to reports/heldout/<set>/first_run.json and never rewritten. Studying its failures to
change the unit turns the set into a regression set (heldout/<set>/STATUS = regression); a new held-out set is
then written from heldout/WRITING_VECTORS.md by someone who has not seen the code.
"""

from __future__ import annotations

import json
import re
import subprocess
from datetime import UTC, datetime
from pathlib import Path

import yaml

from .icd import ROOT
from .qual.evidence import build_dir

HELDOUT = ROOT / "heldout"


def _first_run(name: str) -> Path:
    return ROOT / "reports" / "heldout" / name / "first_run.json"


def _decode(icd: str, message: str, payload_hex: str) -> dict | None:
    p = subprocess.run([str(build_dir() / "iqkctl"), "codec", "decode", icd, message, payload_hex.replace(" ", "")],
                       capture_output=True, text=True)
    if p.returncode != 0:
        return None
    out: dict = {}
    for line in p.stdout.splitlines():
        k, _, v = line.partition("=")
        m = re.fullmatch(r"(\w+)\.(\d+)\.(\w+)", k)
        if m:
            arr = out.setdefault(m.group(1), [])
            i = int(m.group(2))
            while len(arr) <= i:
                arr.append({})
            arr[i][m.group(3)] = int(v)
        else:
            out[k] = int(v)
    return out


def _vector(case: dict) -> dict:
    got = _decode(case["icd"], case["message"], str(case["payload_hex"]))
    want = case["expect"]
    if want == "reject":
        ok = got is None
        detail = "" if ok else "decoded a payload the set expects to be rejected"
    else:
        exp = dict(case.get("fields") or {})
        # A message whose array is empty may omit it from the decoder's output.
        if got is not None:
            for k, v in exp.items():
                if isinstance(v, list) and not v:
                    got.setdefault(k, [])
        ok = got is not None and got == exp
        if got is None:
            detail = "rejected a payload the set expects to decode"
        elif not ok:
            diff = {k: (exp.get(k), got.get(k)) for k in set(exp) | set(got) if exp.get(k) != got.get(k)}
            detail = f"field differences (expected, decoded): {diff}"
        else:
            detail = ""
    return {"name": case["name"], "kind": "vector", "pass": ok, "detail": detail}


def _plan(path: Path) -> dict:
    text = path.read_text()
    m = re.search(r"^#\s*expect:\s*(accept|reject)\s*(.*)$", text, flags=re.M)
    if not m:
        return {"name": path.name, "kind": "plan", "pass": False, "detail": "no '# expect:' line"}
    want_accept = m.group(1) == "accept"
    want_rules = set(re.findall(r"PLN-\d+", m.group(2)))
    p = subprocess.run([str(build_dir() / "iqkctl"), "validate", str(path)], capture_output=True, text=True)
    lines = [json.loads(x) for x in p.stdout.splitlines() if x.strip()]
    rules = {x["rule"] for x in lines if "rule" in x}
    accepted = p.returncode == 0
    if want_accept:
        ok = accepted
        detail = "" if ok else f"refused with {sorted(rules)}"
    else:
        ok = not accepted and rules == want_rules
        if accepted:
            detail = "accepted a plan the set expects to be refused"
        elif rules != want_rules:
            detail = f"refused, but with {sorted(rules)} instead of {sorted(want_rules)}"
        else:
            detail = ""
    return {"name": path.name, "kind": "plan", "pass": ok, "detail": detail,
            "refused_as_expected": (not accepted) == (not want_accept)}


def run(name: str) -> dict:
    base = HELDOUT / name
    status = (base / "STATUS").read_text().strip() if (base / "STATUS").exists() else "heldout"
    out = _first_run(name)
    if out.exists():
        raise SystemExit(f"{out.relative_to(ROOT)} exists: the first run is never rewritten")
    if status != "heldout":
        raise SystemExit(f"set {name} is {status}; write a new held-out set")
    cases = yaml.safe_load((base / "vectors.yaml").read_text())["cases"] if (base / "vectors.yaml").exists() else []
    results = [_vector(c) for c in cases] + [_plan(p) for p in sorted((base / "plans").glob("*.plan"))]
    vec = [r for r in results if r["kind"] == "vector"]
    pla = [r for r in results if r["kind"] == "plan"]
    summary = {
        "set": name,
        "recorded_at": datetime.now(UTC).isoformat(timespec="seconds"),
        "vectors": len(vec),
        "vectors_pass": sum(r["pass"] for r in vec),
        "plans": len(pla),
        "plans_pass": sum(r["pass"] for r in pla),
        "plans_accept_refuse_right": sum(r.get("refused_as_expected", r["pass"]) for r in pla),
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"summary": summary, "results": results}, indent=1) + "\n")
    return summary
