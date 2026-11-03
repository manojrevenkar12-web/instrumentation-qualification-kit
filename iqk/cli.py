"""iqk: the engineer's command line.

iqk icd generate | check            regenerate codecs and ICD documents / fail if committed ones drift
iqk qual evidence [--no-system]     run unit and system tests, record what each claims to verify
iqk qual mutate [--only SRS-x,...]  prove traceability links with mutants
iqk qual matrix                     traceability matrix, verification plan, gate
iqk qual impact <base> [<head>]     requirements a change puts back to "to be re-verified"
iqk qual release --version V        evidence bundle, configuration manifest, SBOM, signature
iqk assurance repro | hardening | sbom | static | sanitize
iqk bench --seconds N [--fault name@from-to ...]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .icd import ROOT

REPORTS = ROOT / "reports"


def _write(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=1, sort_keys=False) + "\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="iqk")
    sub = ap.add_subparsers(dest="cmd", required=True)
    icd = sub.add_parser("icd")
    icd.add_argument("action", choices=["generate", "check"])
    q = sub.add_parser("qual")
    q.add_argument("action", choices=["evidence", "mutate", "matrix", "impact", "release"])
    q.add_argument("--no-system", action="store_true")
    q.add_argument("--only", default="")
    q.add_argument("--version", default="")
    q.add_argument("--key", default="")
    q.add_argument("refs", nargs="*")
    a = sub.add_parser("assurance")
    a.add_argument("action", choices=["repro", "hardening", "sbom", "static", "sanitize"])
    h = sub.add_parser("heldout")
    h.add_argument("action", choices=["run"])
    h.add_argument("set")
    ag = sub.add_parser("agents")
    ag.add_argument("--only", default="")
    ag.add_argument("--models", default="haiku,sonnet")
    b = sub.add_parser("bench")
    b.add_argument("--seconds", type=float, default=20)
    b.add_argument("--plan", default=str(ROOT / "examples" / "plans" / "bench.plan"))
    b.add_argument("--fault", action="append", default=[])
    b.add_argument("--report", default="", help="save a summary as reports/bench/<name>.json")
    b.add_argument("--seed", type=int, default=7)
    sub.add_parser("report")
    args = ap.parse_args(argv)

    if args.cmd == "icd":
        from . import icdgen

        if args.action == "generate":
            for p in icdgen.write():
                print(f"wrote {p.relative_to(ROOT)}")
            return 0
        drift = icdgen.drift()
        for p in drift:
            print(f"out of date: {p.relative_to(ROOT)}")
        return 1 if drift else 0

    if args.cmd == "qual":
        if args.action == "evidence":
            from .qual import evidence

            res = evidence.run(ROOT / "work" / "evidence", system=not args.no_system)
            _write(REPORTS / "qualification" / "tests.json", evidence.to_json(res))
            bad = [r.id for r in res if not r.passed]
            print(f"{len(res)} tests, {len(bad)} failing" + (f": {bad}" if bad else ""))
            return 1 if bad else 0
        if args.action == "mutate":
            from .qual import mutate

            only = {x for x in args.only.split(",") if x} or None
            res = mutate.prove(ROOT / "work" / "mutation", only)
            path = REPORTS / "qualification" / "mutants.json"
            if only and path.exists():  # merge a partial run into the full record
                old = [r for r in json.loads(path.read_text()) if r["requirement"] not in only]
                data = old + mutate.to_json(res)
            else:
                data = mutate.to_json(res)
            _write(path, data)
            return 0
        if args.action == "matrix":
            from .qual import matrix

            m = matrix.build()
            matrix.write(m)
            print(json.dumps(m["summary"], indent=1))
            return 0 if m["summary"]["gate"]["pass"] else 1
        if args.action == "impact":
            from .qual import impact

            base = args.refs[0] if args.refs else "HEAD~1"
            head = args.refs[1] if len(args.refs) > 1 else "HEAD"
            print(json.dumps(impact.impact(base, head), indent=1))
            return 0
        if args.action == "release":
            from .qual import release

            out = release.bundle(args.version, Path(args.key) if args.key else None)
            print(json.dumps(out, indent=1))
            return 0 if out["pass"] else 1

    if args.cmd == "assurance":
        from .assurance import checks

        fn = {
            "repro": checks.reproducible,
            "hardening": checks.hardening,
            "sbom": checks.sbom,
            "static": checks.static,
            "sanitize": checks.sanitize,
        }[args.action]
        result = fn()
        _write(REPORTS / "assurance" / f"{args.action}.json", result)
        print(json.dumps({k: v for k, v in result.items() if k != "details"}, indent=1))
        return 0 if result.get("pass") else 1

    if args.cmd == "heldout":
        from . import heldout

        print(json.dumps(heldout.run(args.set), indent=1))
        return 0

    if args.cmd == "agents":
        from .agents import experiment

        reqs = [x for x in args.only.split(",") if x] or None
        drafts = experiment.run(Path("/tmp/iqk-agents"), reqs, args.models.split(","))
        path = REPORTS / "agents" / "experiment.json"
        old = json.loads(path.read_text())["drafts"] if path.exists() and reqs else []
        keep = [
            d for d in old if (d["requirement"], d["model"]) not in {(x["requirement"], x["model"]) for x in drafts}
        ]
        all_drafts = keep + drafts
        _write(path, {"summary": experiment.summarize(all_drafts), "drafts": all_drafts})
        print(json.dumps(experiment.summarize(all_drafts), indent=1))
        return 0

    if args.cmd == "bench":
        from .bench import Bench, Fault

        faults = []
        for f in args.fault:
            name, _, span = f.partition("@")
            lo, _, hi = span.partition("-")
            faults.append(Fault(name, float(lo or 0), float(hi or 1e9)))
        r = Bench(ROOT / "work" / "bench", Path(args.plan).read_text(), faults, seed=args.seed).run(args.seconds)
        if args.report:
            from .report import bench_summary

            s = bench_summary(r, args.report, faults)
            _write(REPORTS / "bench" / f"{args.report}.json", s)
            print(json.dumps({k: v for k, v in s.items() if k != "time_error_histogram"}, indent=1))
        else:
            print(json.dumps(r.status, indent=1))
        return 0

    if args.cmd == "report":
        from .report import write

        d = write()
        print(f"reports/console.json: {len(d['traceability']['requirements'])} requirements")
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
