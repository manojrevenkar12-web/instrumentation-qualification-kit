"""Software assurance checks, each producing a report the traceability matrix reads.

reproducible  two builds of the same sources in different directories give identical binaries
hardening     the shipped binaries are PIE, full RELRO, NX stack, stack protector, FORTIFY and CET marked
static        clang-tidy and cppcheck report nothing in shipped code
sanitize      the unit tests pass under AddressSanitizer and UndefinedBehaviorSanitizer
sbom          a CycloneDX 1.5 bill of materials for the unit software and its tools
"""

from __future__ import annotations

import hashlib
import json
import platform
import re
import shutil
import subprocess
import tempfile
import uuid
from datetime import UTC, datetime
from importlib import metadata
from pathlib import Path

from ..icd import ROOT
from ..qual.evidence import build_dir

SHIPPED = ["daud", "iqkctl"]


def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def _run(args: list[str], cwd: Path | None = None, timeout: int = 1800) -> subprocess.CompletedProcess:
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=timeout)


def _copy_sources(dst: Path) -> None:
    dst.mkdir(parents=True)
    shutil.copy2(ROOT / "CMakeLists.txt", dst / "CMakeLists.txt")
    shutil.copytree(ROOT / "cpp", dst / "cpp")


def reproducible() -> dict:
    digests = []
    with tempfile.TemporaryDirectory() as t:
        for name in ("first-build", "second-build-elsewhere"):
            src = Path(t) / name / "src"
            _copy_sources(src)
            b = Path(t) / name / "build"
            _run(
                [
                    "cmake",
                    "-S",
                    str(src),
                    "-B",
                    str(b),
                    "-G",
                    "Ninja",
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                    "-DIQK_TESTS=OFF",
                ]
            )
            r = _run(["ninja", "-C", str(b), *SHIPPED])
            if r.returncode != 0:
                return {"pass": False, "error": r.stdout[-2000:]}
            digests.append({x: sha256(b / x) for x in SHIPPED})
    same = digests[0] == digests[1]
    return {"pass": same, "binaries": digests[0], "second": digests[1], "compiler": _compiler()}


def _compiler() -> str:
    return _run(["c++", "--version"]).stdout.splitlines()[0]


def hardening() -> dict:
    results = {}
    for x in SHIPPED:
        p = build_dir() / x
        h = _run(["readelf", "-h", str(p)]).stdout
        lp = _run(["readelf", "-lW", str(p)]).stdout
        dyn = _run(["readelf", "-dW", str(p)]).stdout
        syms = _run(["readelf", "--dyn-syms", "-W", str(p)]).stdout
        notes = _run(["readelf", "-nW", str(p)]).stdout
        stack = re.search(r"GNU_STACK\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+(\S+)", lp)
        checks = {
            "pie": "DYN (Position-Independent Executable" in h or "Type:                              DYN" in h,
            "full_relro": "GNU_RELRO" in lp and "BIND_NOW" in dyn,
            "nx_stack": bool(stack) and "E" not in stack.group(1),
            "stack_protector": "__stack_chk_fail" in syms,
            "fortify": bool(re.search(r"__(?!stack_chk)\w+_chk@", syms)),
            "cet_ibt_shstk": "IBT" in notes and "SHSTK" in notes,
        }
        results[x] = checks
    return {"pass": all(all(v.values()) for v in results.values()), "binaries": results}


def static() -> dict:
    cc = build_dir() / "compile_commands.json"
    files = sorted(str(p) for p in (ROOT / "cpp" / "src").glob("*.cpp")) + sorted(
        str(p) for p in (ROOT / "cpp" / "apps").glob("*.cpp")
    )
    tidy = _run(["clang-tidy", "-p", str(cc.parent), "--quiet", *files], timeout=3600)
    tidy_findings = [line for line in tidy.stdout.splitlines() if re.search(r": (warning|error): ", line)]
    cpp = _run(
        [
            "cppcheck",
            "--quiet",
            "--std=c++20",
            "--enable=warning,performance,portability",
            "--inline-suppr",
            "--error-exitcode=1",
            "-I",
            str(ROOT / "cpp" / "include"),
            "-I",
            str(ROOT / "cpp" / "gen"),
            "--suppress=missingIncludeSystem",
            str(ROOT / "cpp" / "src"),
            str(ROOT / "cpp" / "apps"),
        ],
        timeout=3600,
    )
    cpp_findings = [line for line in cpp.stderr.splitlines() if line.strip()]
    return {
        "pass": not tidy_findings and cpp.returncode == 0,
        "clang_tidy_findings": len(tidy_findings),
        "cppcheck_findings": len(cpp_findings),
        "details": {"clang_tidy": tidy_findings[:50], "cppcheck": cpp_findings[:50]},
    }


def sanitize() -> dict:
    b = ROOT / "work" / "asan"
    _run(["cmake", "-S", str(ROOT), "-B", str(b), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", "-DIQK_SANITIZE=ON"])
    r = _run(["ninja", "-C", str(b), "iqk_tests"])
    if r.returncode != 0:
        return {"pass": False, "error": r.stdout[-2000:]}
    t = _run([str(b / "iqk_tests"), "--gtest_brief=1"], timeout=1800)
    tail = (t.stdout + t.stderr).splitlines()[-5:]
    return {"pass": t.returncode == 0, "tail": tail}


def _source_hash() -> str:
    files = subprocess.run(
        ["git", "ls-files", "CMakeLists.txt", "cpp", "icd", "config"], cwd=ROOT, capture_output=True, text=True
    ).stdout.split()
    h = hashlib.sha256()
    for f in sorted(files):
        h.update(f.encode() + b"\0" + (ROOT / f).read_bytes())
    return h.hexdigest()


def sbom() -> dict:
    version = ".".join(
        str(v) for v in __import__("yaml").safe_load((ROOT / "config" / "unit.yaml").read_text())["unit"]["sw_version"]
    )
    openssl = _run(["openssl", "version"]).stdout.split()[1] if shutil.which("openssl") else "unknown"
    components = [
        {
            "type": "application",
            "bom-ref": "iqk-daud",
            "name": "daud",
            "version": version,
            "hashes": [{"alg": "SHA-256", "content": sha256(build_dir() / "daud")}]
            if (build_dir() / "daud").exists()
            else [],
        },
        {
            "type": "application",
            "bom-ref": "iqk-iqkctl",
            "name": "iqkctl",
            "version": version,
            "hashes": [{"alg": "SHA-256", "content": sha256(build_dir() / "iqkctl")}]
            if (build_dir() / "iqkctl").exists()
            else [],
        },
        {
            "type": "library",
            "bom-ref": "openssl",
            "name": "OpenSSL libcrypto",
            "version": openssl,
            "purl": f"pkg:generic/openssl@{openssl}",
            "scope": "required",
        },
        {
            "type": "library",
            "bom-ref": "libstdcxx",
            "name": "libstdc++",
            "version": _gcc_version(),
            "scope": "required",
        },
        {
            "type": "library",
            "bom-ref": "googletest",
            "name": "GoogleTest",
            "version": _deb_version("libgtest-dev"),
            "scope": "excluded",
            "description": "test only, not shipped",
        },
    ]
    for dist in ("PyYAML", "cryptography", "hypothesis", "pytest"):
        try:
            v = metadata.version(dist)
        except metadata.PackageNotFoundError:
            continue
        components.append(
            {
                "type": "library",
                "bom-ref": f"py-{dist.lower()}",
                "name": dist,
                "version": v,
                "purl": f"pkg:pypi/{dist.lower()}@{v}",
                "scope": "excluded",
                "description": "host tooling (generator, emulators, qualification), not shipped",
            }
        )
    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{uuid.uuid5(uuid.NAMESPACE_URL, 'iqk-' + _source_hash())}",
        "version": 1,
        "metadata": {
            "timestamp": datetime.now(UTC).isoformat(timespec="seconds"),
            "tools": {"components": [{"type": "application", "name": "iqk assurance sbom"}]},
            "component": {
                "type": "firmware",
                "bom-ref": "dau-software",
                "name": "DAU software",
                "version": version,
                "hashes": [{"alg": "SHA-256", "content": _source_hash()}],
            },
            "properties": [
                {"name": "build:compiler", "value": _compiler()},
                {"name": "build:platform", "value": platform.platform()},
            ],
        },
        "components": components,
        "dependencies": [
            {"ref": "iqk-daud", "dependsOn": ["openssl", "libstdcxx"]},
            {"ref": "iqk-iqkctl", "dependsOn": ["openssl", "libstdcxx"]},
        ],
    }
    out = ROOT / "reports" / "assurance" / "sbom.cdx.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(bom, indent=1) + "\n")
    shipped_ok = all(
        c.get("version") and c.get("version") != "unknown" for c in components if c.get("scope") != "excluded"
    )
    binaries_hashed = all(c["hashes"] for c in components[:2])
    return {
        "pass": shipped_ok and binaries_hashed,
        "components": len(components),
        "file": "reports/assurance/sbom.cdx.json",
        "sha256": sha256(out),
    }


def _gcc_version() -> str:
    m = re.search(r"(\d+\.\d+\.\d+)", _compiler())
    return m.group(1) if m else "unknown"


def _deb_version(pkg: str) -> str:
    r = _run(["dpkg-query", "-W", "-f=${Version}", pkg]) if shutil.which("dpkg-query") else None
    return r.stdout.strip() if r and r.returncode == 0 and r.stdout.strip() else "unknown"
