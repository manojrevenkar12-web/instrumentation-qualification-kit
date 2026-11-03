import type { Tone } from "../theme/tokens";
import type { Requirement } from "./console";

/** The tone and plain-language label of a requirement's status. */
export function statusTone(status: string): Tone {
  if (status === "verified") return "proven";
  if (status.startsWith("verified")) return "system";
  if (status === "weak" || status === "mutation not run" || status === "not run") return "weak";
  if (status === "untested" || status === "failing" || status === "failed" || status === "mutant broken") return "failed";
  return "neutral";
}

export function statusLabel(status: string): string {
  switch (status) {
    case "verified":
      return "Verified: every mutant caught";
    case "verified, system level (no mutants)":
      return "Verified on the bench (system level, no mutants)";
    case "weak":
      return "Weak: a mutant survives its tests";
    case "untested":
      return "No test claims it";
    case "failing":
      return "A linked test fails";
    case "not run":
      return "Check not run";
    case "failed":
      return "Check failed";
    default:
      return status.charAt(0).toUpperCase() + status.slice(1);
  }
}

export function caught(r: Requirement): number {
  return r.mutants.filter((m) => m.caught_by.length > 0).length;
}

/** "gtest:Suite.Name" -> "Suite.Name", "pytest:tests/system/test_bench.py::name" -> "test_bench.py · name" */
export function testName(id: string): string {
  const [kind, rest] = id.split(/:(.*)/s);
  if (kind === "pytest") {
    const [file, fn] = rest.split("::");
    return `${file.split("/").pop()} ${fn}`;
  }
  return rest;
}

export function pct(n: number, d: number): string {
  return d === 0 ? "–" : `${Math.round((100 * n) / d)}%`;
}

export function us(v: number | null | undefined): string {
  if (v === null || v === undefined) return "–";
  return v >= 1000 ? `${(v / 1000).toFixed(2)} ms` : `${Math.round(v)} µs`;
}
