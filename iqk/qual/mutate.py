"""Mutation proof of traceability links.

For each requirement, each mutant (qual/mutants.yaml) is applied to a scratch copy of the sources, the unit
tests are rebuilt and run, and the tests that fail are recorded. A link "test T verifies requirement R" is
proven when T fails on at least one of R's mutants; a requirement is proven when every one of its mutants is
caught by a test linked to it.
"""

from __future__ import annotations

import shutil
import subprocess
import time
import xml.etree.ElementTree as ET
from dataclasses import asdict, dataclass, field
from pathlib import Path

import yaml

from ..icd import ROOT

MUTANTS = ROOT / "qual" / "mutants.yaml"


@dataclass
class Mutant:
    requirement: str
    name: str
    file: str
    find: str = ""
    replace: str = ""
    after: str = ""
    edits: list[dict] = field(default_factory=list)  # several edits in one file, applied in order


@dataclass
class MutantResult:
    requirement: str
    name: str
    file: str
    outcome: str  # killed | survived | stillborn | invalid
    failed_tests: list[str] = field(default_factory=list)
    seconds: float = 0.0
    detail: str = ""


def load() -> list[Mutant]:
    raw = yaml.safe_load(MUTANTS.read_text()) or {}
    return [Mutant(requirement=req, **m) for req, ms in raw.items() for m in ms]


def _edit(text: str, find: str, replace: str, after: str) -> str | None:
    if after:
        if text.count(after) != 1:
            return None
        pos = text.find(find, text.index(after))
        if pos < 0:
            return None
    else:
        if text.count(find) != 1:
            return None
        pos = text.index(find)
    return text[:pos] + replace + text[pos + len(find) :]


def apply(text: str, m: Mutant) -> str | None:
    """The mutated text, or None when a `find` is not found exactly once (in the region after `after`)."""
    edits = m.edits or [{"find": m.find, "replace": m.replace, "after": m.after}]
    for e in edits:
        out = _edit(text, e["find"], e["replace"], e.get("after", ""))
        if out is None:
            return None
        text = out
    return text


def _failed(xml: Path) -> list[str]:
    if not xml.exists():
        return []
    out = []
    for case in ET.parse(xml).getroot().iter("testcase"):
        if case.find("failure") is not None:
            out.append(f"gtest:{case.get('classname')}.{case.get('name')}")
    return out


class Workspace:
    def __init__(self, work: Path) -> None:
        self.src = work / "src"
        self.build = work / "build"
        self.xml = work / "result.xml"
        if self.src.exists():
            shutil.rmtree(self.src)
        self.src.mkdir(parents=True)
        shutil.copy2(ROOT / "CMakeLists.txt", self.src / "CMakeLists.txt")
        shutil.copytree(ROOT / "cpp", self.src / "cpp")
        subprocess.run(
            [
                "cmake",
                "-S",
                str(self.src),
                "-B",
                str(self.build),
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=None",
                "-DCMAKE_CXX_FLAGS=-O1",
            ],
            check=True,
            capture_output=True,
        )
        self.compile()

    def compile(self) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["ninja", "-C", str(self.build), "iqk_tests"], capture_output=True, text=True, timeout=1200
        )

    def run_tests(self) -> tuple[int, list[str]]:
        if self.xml.exists():
            self.xml.unlink()
        try:
            p = subprocess.run(
                [str(self.build / "iqk_tests"), f"--gtest_output=xml:{self.xml}"], capture_output=True, timeout=120
            )
            rc, failed = p.returncode, _failed(self.xml)
        except subprocess.TimeoutExpired:
            rc, failed = -1, []
        if rc != 0 and not failed:
            # The run crashed or hung before writing its report: find the responsible tests one at a time.
            failed = self._one_by_one()
        return rc, failed

    def _one_by_one(self) -> list[str]:
        listing = subprocess.run([str(self.build / "iqk_tests"), "--gtest_list_tests"], capture_output=True, text=True)
        tests, suite = [], ""
        for line in listing.stdout.splitlines():
            if not line.startswith(" "):
                suite = line.strip()
            elif line.strip():
                tests.append(suite + line.strip().split()[0])
        bad = []
        for t in tests:
            try:
                r = subprocess.run(
                    [str(self.build / "iqk_tests"), f"--gtest_filter={t}"], capture_output=True, timeout=30
                )
                if r.returncode != 0:
                    bad.append(f"gtest:{t}")
            except subprocess.TimeoutExpired:
                bad.append(f"gtest:{t}")
        return bad


def prove(work: Path, only: set[str] | None = None, log=print) -> list[MutantResult]:
    ws = Workspace(work)
    rc, failed = ws.run_tests()
    if rc != 0:
        raise RuntimeError(f"the unit tests fail before any mutation: {failed}")
    results = []
    for m in load():
        if only and m.requirement not in only:
            continue
        t0 = time.monotonic()
        path = ws.src / m.file
        if not path.exists():
            results.append(MutantResult(m.requirement, m.name, m.file, "invalid", detail="no such file"))
            continue
        pristine = path.read_text()
        mutated = apply(pristine, m)
        if mutated is None:
            results.append(MutantResult(m.requirement, m.name, m.file, "invalid", detail="find text is not unique"))
            continue
        try:
            path.write_text(mutated)
            b = ws.compile()
            if b.returncode != 0:
                tail = "\n".join(line for line in b.stdout.splitlines() if "error" in line)[:600]
                r = MutantResult(m.requirement, m.name, m.file, "stillborn", detail=tail)
            else:
                rc, failed = ws.run_tests()
                r = MutantResult(m.requirement, m.name, m.file, "killed" if rc != 0 else "survived", failed)
        finally:
            path.write_text(pristine)
        r.seconds = round(time.monotonic() - t0, 1)
        log(f"{m.requirement} {m.name}: {r.outcome} ({len(r.failed_tests)} tests failed, {r.seconds}s)")
        results.append(r)
    ws.compile()  # leave the workspace built from pristine sources
    return results


def to_json(results: list[MutantResult]) -> list[dict]:
    return [asdict(r) for r in results]
