"""Where do coding agents help in qualification work, and where don't they?

For a sample of requirements, a Claude Code session is asked to draft GoogleTest tests from the requirement
text, the headers and the ICDs, in a workspace without the project's own tests, mutants or reports. Each
draft is then measured exactly like a human test: does it compile, does it pass on the unit, and does it
fail on the requirement's mutants. A draft that passes but catches no mutant is the dangerous case: it looks
like evidence and is not.

Drafts are proposals. They enter the evidence only when an engineer reviews them and the mutation proof
holds; an agent has no signing key (see iqk.qual.release).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, dataclass
from pathlib import Path

import yaml

from ..icd import ROOT
from ..qual import mutate

REQUIREMENTS = [
    "SRS-011",
    "SRS-013",
    "SRS-021",
    "SRS-022",
    "SRS-030",
    "SRS-043",
    "SRS-045",
    "SRS-047",
    "SRS-048",
    "SRS-050",
    "SRS-055",
    "SRS-062",
    "SRS-065",
    "SRS-081",
    "SRS-092",
    "SRS-094",
]
MODELS = ["haiku", "sonnet"]
BUDGET_USD = 1.0
TIMEOUT_S = 900

PROMPT = """You are a verification engineer on a flight-test data acquisition unit (C++20).

Write GoogleTest tests that verify this software requirement:

{rid} {title}
{text}

Put them in cpp/tests/agent_test.cpp. Start each test with IQK_VERIFIES("{rid}"); (cpp/tests/support.hpp
defines it and has helpers; cpp/tests/fake_module.hpp is an in-process acquisition module). Read the public
headers in cpp/include/iqk/ and cpp/gen/iqk/, the configuration in config/unit.yaml and the interface documents
in docs/icd/ to learn the API. Test the behaviour the requirement states, including its boundaries; a test that
would still pass if the requirement were broken is worthless.

Build and run with: ninja -C build iqk_tests && ./build/iqk_tests --gtest_brief=1
Do not change anything outside cpp/tests/agent_test.cpp. When the tests compile and pass, stop and reply with
one line naming the tests you wrote."""


@dataclass
class Draft:
    requirement: str
    model: str
    compiled: bool
    passed: bool
    tests: int
    mutants: int
    killed: int
    cost_usd: float
    seconds: float
    turns: int
    denied: int
    note: str = ""


def _workspace(base: Path) -> Path:
    ws = base / "ws"
    if ws.exists():
        shutil.rmtree(ws)
    ws.mkdir(parents=True)
    shutil.copy2(ROOT / "CMakeLists.txt", ws)
    shutil.copytree(ROOT / "cpp", ws / "cpp", ignore=shutil.ignore_patterns("test_*.cpp"))
    shutil.copytree(ROOT / "config", ws / "config")
    (ws / "docs").mkdir()
    shutil.copytree(ROOT / "docs" / "icd", ws / "docs" / "icd")
    # A placeholder so the test binary links before the agent writes anything.
    (ws / "cpp" / "tests" / "agent_test.cpp").write_text('#include "support.hpp"\n')
    subprocess.run(
        [
            "cmake",
            "-S",
            str(ws),
            "-B",
            str(ws / "build"),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=None",
            "-DCMAKE_CXX_FLAGS=-O1",
        ],
        check=True,
        capture_output=True,
    )
    subprocess.run(["ninja", "-C", str(ws / "build"), "iqk_tests"], capture_output=True)
    return ws


def _session(ws: Path, model: str, prompt: str, transcript: Path) -> dict:
    allowed = [
        "Read",
        "Edit",
        "Write",
        "Glob",
        "Grep",
        "Bash(ninja -C build:*)",
        "Bash(./build/iqk_tests:*)",
        "Bash(ls:*)",
    ]
    # --restricted confines the file tools to the workspace: the session cannot read the project's own tests,
    # mutants or reports, which live outside it.
    cmd = [
        "claude",
        "-p",
        "--restricted",
        "--model",
        model,
        "--output-format",
        "stream-json",
        "--verbose",
        "--permission-mode",
        "dontAsk",
        "--max-budget-usd",
        str(BUDGET_USD),
        "--no-session-persistence",
        "--strict-mcp-config",
        "--tools",
        "Read,Edit,Write,Glob,Grep,Bash",
        "--allowedTools",
        *allowed,
    ]
    env = {"HOME": os.environ["HOME"], "PATH": os.environ["PATH"]}
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, cwd=ws, input=prompt, capture_output=True, text=True, timeout=TIMEOUT_S, env=env)
        out = p.stdout
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
    transcript.write_text(out)
    result = {}
    for line in out.splitlines():
        try:
            ev = json.loads(line)
        except json.JSONDecodeError:
            continue
        if ev.get("type") == "result":
            result = ev
    return {
        "cost": float(result.get("total_cost_usd") or 0.0),
        "turns": int(result.get("num_turns") or 0),
        "denied": len(result.get("permission_denials") or []),
        "seconds": round(time.monotonic() - t0, 1),
    }


def _tests_in(path: Path) -> list[str]:
    return [f"{a}.{b}" for a, b in re.findall(r"^TEST(?:_F)?\((\w+),\s*(\w+)\)", path.read_text(), flags=re.M)]


def _evaluate(draft: Path, rid: str, work: Path) -> tuple[bool, bool, int, int, int]:
    """compiled, passed on the unit, number of tests, mutants of the requirement, mutants killed."""
    ev = work / "eval"
    if ev.exists():
        shutil.rmtree(ev)
    ev.mkdir(parents=True)
    shutil.copy2(ROOT / "CMakeLists.txt", ev)
    shutil.copytree(ROOT / "cpp", ev / "cpp", ignore=shutil.ignore_patterns("test_*.cpp"))
    shutil.copy2(draft, ev / "cpp" / "tests" / "agent_test.cpp")
    b = ev / "build"
    subprocess.run(
        ["cmake", "-S", str(ev), "-B", str(b), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=None", "-DCMAKE_CXX_FLAGS=-O1"],
        check=True,
        capture_output=True,
    )
    tests = _tests_in(draft)
    if not tests or subprocess.run(["ninja", "-C", str(b), "iqk_tests"], capture_output=True).returncode != 0:
        return False, False, len(tests), 0, 0
    flt = "--gtest_filter=" + ":".join(tests)
    if subprocess.run([str(b / "iqk_tests"), flt], capture_output=True, timeout=120).returncode != 0:
        return True, False, len(tests), 0, 0
    muts = [m for m in mutate.load() if m.requirement == rid]
    killed = 0
    for m in muts:
        path = ev / m.file
        pristine = path.read_text()
        mutated = mutate.apply(pristine, m)
        if mutated is None:
            continue
        path.write_text(mutated)
        try:
            if subprocess.run(["ninja", "-C", str(b), "iqk_tests"], capture_output=True).returncode == 0:
                try:
                    rc = subprocess.run([str(b / "iqk_tests"), flt], capture_output=True, timeout=120).returncode
                except subprocess.TimeoutExpired:
                    rc = -1
                killed += rc != 0
        finally:
            path.write_text(pristine)
    return True, True, len(tests), len(muts), killed


def run_one(rid: str, model: str, base: Path) -> Draft:
    req = next(
        r for r in yaml.safe_load((ROOT / "requirements" / "srs.yaml").read_text())["requirements"] if r["id"] == rid
    )
    work = base / f"{rid}-{model}"
    ws = _workspace(work)
    s = _session(ws, model, PROMPT.format(rid=rid, title=req["title"], text=req["text"]), work / "transcript.jsonl")
    draft = ws / "cpp" / "tests" / "agent_test.cpp"
    keep = ROOT / "reports" / "agents" / "drafts" / f"{rid}-{model}.cpp"
    keep.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(draft, keep)
    compiled, passed, n, total, killed = _evaluate(draft, rid, work)
    return Draft(rid, model, compiled, passed, n, total, killed, s["cost"], s["seconds"], s["turns"], s["denied"])


def run(
    base: Path, requirements: list[str] | None = None, models: list[str] | None = None, workers: int = 2
) -> list[dict]:
    jobs = [(r, m) for r in (requirements or REQUIREMENTS) for m in (models or MODELS)]
    out: list[Draft] = []
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for d in pool.map(lambda j: run_one(j[0], j[1], base), jobs):
            print(
                f"{d.requirement} {d.model}: compiled={d.compiled} passed={d.passed} killed {d.killed}/{d.mutants} "
                f"${d.cost_usd:.3f} {d.seconds}s",
                flush=True,
            )
            out.append(d)
    return [asdict(d) for d in out]


def summarize(drafts: list[dict]) -> dict:
    by = {}
    for model in sorted({d["model"] for d in drafts}):
        mine = [d for d in drafts if d["model"] == model]
        passing = [d for d in mine if d["passed"]]
        proven = [d for d in passing if d["mutants"] and d["killed"] == d["mutants"]]
        hollow = [d for d in passing if d["mutants"] and d["killed"] == 0]
        by[model] = {
            "drafts": len(mine),
            "compiled": sum(d["compiled"] for d in mine),
            "passed": len(passing),
            "proven": len(proven),
            "partly_proven": len(passing) - len(proven) - len(hollow),
            "hollow": len(hollow),
            "mutants": sum(d["mutants"] for d in passing),
            "killed": sum(d["killed"] for d in passing),
            "cost_usd": round(sum(d["cost_usd"] for d in mine), 3),
            "median_seconds": sorted(d["seconds"] for d in mine)[len(mine) // 2] if mine else None,
        }
    return by
