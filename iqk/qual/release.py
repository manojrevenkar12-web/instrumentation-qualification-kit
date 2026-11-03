"""Release evidence: a configuration manifest that names every configuration item by hash, the evidence that
goes with it, and signatures.

The build signature is made by the pipeline. The release is not qualified until an engineer signs the same
manifest with their own key (`iqk qual release --approve --key <engineer seed>`): coding agents may draft
tests, emulators and evidence, but they hold no signing key and cannot approve a release.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
from datetime import UTC, datetime
from pathlib import Path

import yaml
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

from ..icd import ROOT
from ..icd import keys as icd_keys
from ..icd import load as load_icd
from .evidence import build_dir

OUT = ROOT / "reports" / "release"
EVIDENCE = [
    "reports/qualification/tests.json",
    "reports/qualification/mutants.json",
    "reports/qualification/traceability.json",
    "reports/assurance/repro.json",
    "reports/assurance/hardening.json",
    "reports/assurance/static.json",
    "reports/assurance/sanitize.json",
    "reports/assurance/sbom.json",
    "reports/assurance/sbom.cdx.json",
    "docs/TRACEABILITY.md",
    "docs/VERIFICATION_PLAN.md",
]


def _sha(p: Path) -> str | None:
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None


def _git(*a: str) -> str:
    return subprocess.run(["git", *a], cwd=ROOT, capture_output=True, text=True).stdout.strip()


def manifest(version: str) -> dict:
    cfg = yaml.safe_load((ROOT / "config" / "unit.yaml").read_text())
    tracked = _git("ls-files", "CMakeLists.txt", "cpp", "icd", "config", "requirements", "qual").split()
    tree = hashlib.sha256()
    for f in sorted(tracked):
        tree.update(f.encode() + b"\0" + (ROOT / f).read_bytes())
    trace = json.loads((ROOT / "reports/qualification/traceability.json").read_text())
    return {
        "product": "DAU software",
        "version": version or ".".join(str(v) for v in cfg["unit"]["sw_version"]),
        "hw_id": f"0x{cfg['unit']['hw_id']:08X}",
        "commit": _git("rev-parse", "HEAD"),
        "created": datetime.now(UTC).isoformat(timespec="seconds"),
        "source_tree_sha256": tree.hexdigest(),
        "configuration_items": {
            **{
                f"icd/{k}.yaml": {
                    "id": load_icd(k).icd,
                    "version": load_icd(k).version,
                    "sha256": _sha(ROOT / f"icd/{k}.yaml"),
                }
                for k in icd_keys()
            },
            "config/unit.yaml": {"sha256": _sha(ROOT / "config/unit.yaml")},
            "requirements/srs.yaml": {
                "version": yaml.safe_load((ROOT / "requirements/srs.yaml").read_text())["version"],
                "sha256": _sha(ROOT / "requirements/srs.yaml"),
            },
        },
        "binaries": {x: _sha(build_dir() / x) for x in ("daud", "iqkctl")},
        "evidence": {e: _sha(ROOT / e) for e in EVIDENCE},
        "gate": trace["summary"]["gate"],
    }


def _canonical(m: dict) -> bytes:
    return json.dumps(m, sort_keys=True, separators=(",", ":")).encode()


def _key(seed_file: Path | None) -> tuple[Ed25519PrivateKey, str]:
    if seed_file is None:
        return Ed25519PrivateKey.generate(), "ephemeral pipeline key (no long-lived secret in CI)"
    return Ed25519PrivateKey.from_private_bytes(bytes.fromhex(seed_file.read_text().strip())), str(seed_file.name)


def bundle(version: str, key: Path | None) -> dict:
    m = manifest(version)
    missing = [e for e, h in m["evidence"].items() if h is None]
    priv, label = _key(key)
    sig = priv.sign(_canonical(m))
    pub = priv.public_key().public_bytes_raw().hex()
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "manifest.json").write_text(json.dumps(m, indent=1) + "\n")
    signature = {
        "role": "build",
        "key": label,
        "public_key": pub,
        "signature": sig.hex(),
        "manifest_sha256": hashlib.sha256(_canonical(m)).hexdigest(),
    }
    (OUT / "build.sig.json").write_text(json.dumps(signature, indent=1) + "\n")
    approvals = sorted(p.name for p in OUT.glob("approval-*.json"))
    status = {
        "pass": not missing and m["gate"]["pass"],
        "version": m["version"],
        "manifest_sha256": signature["manifest_sha256"],
        "missing_evidence": missing,
        "gate": m["gate"]["pass"],
        "build_signature": True,
        "engineer_approval": "present" if approvals else "awaiting engineer sign-off",
    }
    (ROOT / "reports" / "assurance").mkdir(parents=True, exist_ok=True)
    (ROOT / "reports" / "assurance" / "release.json").write_text(json.dumps(status, indent=1) + "\n")
    return status


def verify(sig_file: Path) -> bool:
    s = json.loads(sig_file.read_text())
    m = json.loads((OUT / "manifest.json").read_text())
    try:
        Ed25519PublicKey.from_public_bytes(bytes.fromhex(s["public_key"])).verify(
            bytes.fromhex(s["signature"]), _canonical(m)
        )
        return True
    except Exception:
        return False
