"""Traceability: requirement -> tests -> results -> mutation proof, the verification plan, and the gate.

Inputs are generated, never written by hand: reports/qualification/tests.json (iqk qual evidence),
reports/qualification/mutants.json (iqk qual mutate) and reports/assurance/*.json (iqk assurance ...).
"""

from __future__ import annotations

import json
from collections import defaultdict
from datetime import UTC, datetime

import yaml

from .. import icdgen
from ..icd import ROOT
from . import mutate

REPORTS = ROOT / "reports"
SRS = ROOT / "requirements" / "srs.yaml"
ASSURANCE = {
    "SRS-110": ["repro"],
    "SRS-111": ["hardening"],
    "SRS-112": ["static", "sanitize"],
    "SRS-113": ["sbom", "release"],
}


def _load(path, default):
    return json.loads(path.read_text()) if path.exists() else default


def requirements() -> list[dict]:
    return yaml.safe_load(SRS.read_text())["requirements"]


def build() -> dict:
    reqs = requirements()
    tests = _load(REPORTS / "qualification" / "tests.json", [])
    mutants = _load(REPORTS / "qualification" / "mutants.json", [])
    declared = defaultdict(list)
    for m in mutate.load():
        declared[m.requirement].append(m.name)
    by_req_tests = defaultdict(list)
    for t in tests:
        for r in t["verifies"]:
            by_req_tests[r].append(t)
    by_req_mut = defaultdict(list)
    for m in mutants:
        by_req_mut[m["requirement"]].append(m)
    known = {r["id"] for r in reqs}

    rows = []
    for r in reqs:
        rid = r["id"]
        linked = by_req_tests.get(rid, [])
        muts = by_req_mut.get(rid, [])
        linked_ids = {t["id"] for t in linked}
        links = []
        for t in linked:
            kills = [m["name"] for m in muts if t["id"] in m["failed_tests"]]
            links.append(
                {
                    "test": t["id"],
                    "passed": t["passed"],
                    "kills": kills,
                    "proof": "proven"
                    if kills
                    else ("system" if t["id"].startswith("pytest:") or not muts else "unproven"),
                }
            )
        mrows = []
        for m in muts:
            caught_by = sorted(set(m["failed_tests"]) & linked_ids)
            other = sorted(set(m["failed_tests"]) - linked_ids)
            mrows.append(
                {"name": m["name"], "outcome": m["outcome"], "caught_by": caught_by, "caught_by_unlinked": other}
            )
        missing_mutant_runs = sorted(set(declared.get(rid, [])) - {m["name"] for m in muts})

        if r["method"] in ("analysis", "inspection"):
            names = ASSURANCE.get(rid, [])
            reports = [_load(REPORTS / "assurance" / f"{n}.json", None) for n in names]
            if not names or any(x is None for x in reports):
                status = "not run"
            else:
                status = "verified" if all(x.get("pass") for x in reports) else "failed"
            evidence = ", ".join(f"reports/assurance/{n}.json" for n in names)
        elif not linked:
            status, evidence = "untested", ""
        elif any(not t["passed"] for t in linked):
            status, evidence = "failing", ""
        elif not declared.get(rid):
            status, evidence = "verified, system level (no mutants)", ""
        elif missing_mutant_runs:
            status, evidence = "mutation not run", ""
        elif any(m["outcome"] in ("stillborn", "invalid") for m in mrows):
            status, evidence = "mutant broken", ""
        elif all(m["caught_by"] for m in mrows):
            status, evidence = "verified", ""
        else:
            status, evidence = "weak", ""
        rows.append(
            {
                "id": rid,
                "title": r["title"],
                "text": r["text"],
                "method": r["method"],
                "cites": r.get("cites", []),
                "status": status,
                "evidence": evidence,
                "links": links,
                "mutants": mrows,
            }
        )
    orphan_claims = sorted({rq for t in tests for rq in t["verifies"] if rq not in known})
    counts = defaultdict(int)
    for row in rows:
        counts[row["status"]] += 1
    all_links = [lk for row in rows for lk in row["links"]]
    all_mut = [m for row in rows for m in row["mutants"]]
    drift = [str(p.relative_to(ROOT)) for p in icdgen.drift()]
    ok_status = {"verified", "verified, system level (no mutants)"}
    gate = {
        "all_requirements_verified": all(row["status"] in ok_status for row in rows),
        "no_failing_tests": all(t["passed"] for t in tests) and bool(tests),
        "every_mutant_caught": all(m["caught_by"] for m in all_mut) and bool(all_mut),
        "no_claims_on_unknown_requirements": not orphan_claims,
        "generated_files_current": not drift,
    }
    gate["pass"] = all(gate.values())
    summary = {
        "requirements": len(rows),
        "by_status": dict(counts),
        "tests": len(tests),
        "tests_passed": sum(t["passed"] for t in tests),
        "links": len(all_links),
        "links_proven": sum(lk["proof"] == "proven" for lk in all_links),
        "links_unproven": sum(lk["proof"] == "unproven" for lk in all_links),
        "links_system": sum(lk["proof"] == "system" for lk in all_links),
        "mutants": len(all_mut),
        "mutants_caught_by_linked_tests": sum(bool(m["caught_by"]) for m in all_mut),
        "mutants_caught_only_by_unlinked_tests": sum(
            not m["caught_by"] and bool(m["caught_by_unlinked"]) for m in all_mut
        ),
        "orphan_claims": orphan_claims,
        "generated_drift": drift,
        "gate": gate,
    }
    return {"generated_at": datetime.now(UTC).isoformat(timespec="seconds"), "summary": summary, "requirements": rows}


def markdown(m: dict) -> str:
    s = m["summary"]
    out = [
        "# Traceability matrix",
        "",
        f"Generated by `iqk qual matrix` ({m['generated_at']}). Do not edit.",
        "",
        f"{s['requirements']} requirements: " + ", ".join(f"{v} {k}" for k, v in sorted(s["by_status"].items())) + ".",
        f"{s['tests_passed']} of {s['tests']} tests pass. {s['links']} requirement-test links: "
        f"{s['links_proven']} proven by a mutant the test catches, {s['links_unproven']} unproven, "
        f"{s['links_system']} system-level.",
        f"{s['mutants_caught_by_linked_tests']} of {s['mutants']} mutants are caught by a test linked to their "
        "requirement.",
        "",
        f"**Gate: {'pass' if s['gate']['pass'] else 'fail'}** "
        + ", ".join(f"{k.replace('_', ' ')}: {'yes' if v else 'no'}" for k, v in s["gate"].items() if k != "pass")
        + ".",
        "",
        "| Requirement | Method | Status | Tests (proof) | Mutants caught |",
        "|---|---|---|---|---|",
    ]
    for r in m["requirements"]:
        tests = "<br>".join(
            f"{lk['test'].split(':', 1)[1]} ({lk['proof']}{'' if lk['passed'] else ', FAILING'})" for lk in r["links"]
        ) or (r["evidence"] or "none")
        muts = f"{sum(bool(x['caught_by']) for x in r['mutants'])}/{len(r['mutants'])}" if r["mutants"] else "-"
        out.append(f"| **{r['id']}** {r['title']} | {r['method']} | {r['status']} | {tests} | {muts} |")
    out.append("")
    return "\n".join(out)


def plan_markdown(m: dict) -> str:
    out = [
        "# Software verification plan",
        "",
        f"Generated by `iqk qual matrix` from requirements/srs.yaml, qual/mutants.yaml and the tests' own claims "
        f"({m['generated_at']}). Do not edit.",
        "",
        "## How each requirement is verified",
        "",
        "- **test**: requirements-based tests, unit (GoogleTest, in-process) or system (the daemon on the SITL bench).",
        "  A test's claim to verify a requirement is evidence only when the test fails on that requirement's mutants.",
        "- **analysis**: a computed check run by CI (reproducible build, static analysis, sanitizers).",
        "- **inspection**: a check of a built artifact (hardening flags in the binaries, the release bundle).",
        "",
        "Pass criteria: every linked test passes on the release build; every mutant of the requirement is caught by a",
        "linked test; analysis and inspection reports pass. Evidence is generated per release and signed.",
        "",
        "## Requirements",
        "",
    ]
    for r in m["requirements"]:
        out.append(f"### {r['id']} {r['title']}")
        out.append("")
        out.append(r["text"])
        out.append("")
        out.append(f"- Method: {r['method']}")
        if r["cites"]:
            out.append(f"- Depends on: {', '.join(r['cites'])}")
        if r["links"]:
            out.append("- Tests: " + ", ".join(f"`{lk['test']}`" for lk in r["links"]))
        if r["mutants"]:
            out.append("- Mutants: " + ", ".join(f"`{x['name']}`" for x in r["mutants"]))
        out.append("")
    return "\n".join(out)


def write(m: dict) -> None:
    (REPORTS / "qualification").mkdir(parents=True, exist_ok=True)
    (REPORTS / "qualification" / "traceability.json").write_text(json.dumps(m, indent=1) + "\n")
    (ROOT / "docs" / "TRACEABILITY.md").write_text(markdown(m))
    (ROOT / "docs" / "VERIFICATION_PLAN.md").write_text(plan_markdown(m))
