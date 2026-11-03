/** Shape of reports/console.json, written by `iqk report`. The console only reads it. */

export interface Link {
  test: string;
  passed: boolean;
  kills: string[];
  proof: "proven" | "unproven" | "system";
}

export interface MutantRow {
  name: string;
  outcome: "killed" | "survived" | "stillborn" | "invalid";
  caught_by: string[];
  caught_by_unlinked: string[];
}

export interface Requirement {
  id: string;
  title: string;
  text: string;
  method: "test" | "analysis" | "inspection";
  cites: string[];
  status: string;
  evidence: string;
  links: Link[];
  mutants: MutantRow[];
}

export interface Gate {
  all_requirements_verified: boolean;
  no_failing_tests: boolean;
  every_mutant_caught: boolean;
  no_claims_on_unknown_requirements: boolean;
  generated_files_current: boolean;
  pass: boolean;
}

export interface TraceSummary {
  requirements: number;
  by_status: Record<string, number>;
  tests: number;
  tests_passed: number;
  links: number;
  links_proven: number;
  links_unproven: number;
  links_system: number;
  mutants: number;
  mutants_caught_by_linked_tests: number;
  gate: Gate;
}

export interface BenchRun {
  name: string;
  seconds: number;
  faults_injected: { name: string; from_s: number; to_s: number | null }[];
  packets: number;
  samples: number;
  crc_errors_at_receiver: number;
  time_source: number;
  time_state: number;
  time_error_us: { n: number; p50: number | null; p95: number | null; p99: number | null; max: number | null };
  time_error_histogram: { edges_us: number[]; counts: number[] };
  time_quality: Record<string, number>;
  faults_logged: Record<string, number>;
}

export interface HeldoutSet {
  summary: { set: string; recorded_at: string; vectors: number; vectors_pass: number; plans: number; plans_pass: number };
  status: string;
  failed: { name: string; kind: string; detail: string }[];
}

export interface AgentModel {
  drafts: number;
  compiled: number;
  passed: number;
  proven: number;
  partly_proven: number;
  hollow: number;
  mutants: number;
  killed: number;
  cost_usd: number;
  median_seconds: number | null;
}

export interface AgentDraft {
  requirement: string;
  model: string;
  compiled: boolean;
  passed: boolean;
  tests: number;
  mutants: number;
  killed: number;
  cost_usd: number;
}

export interface Console {
  generated_at: string;
  unit: { sw_version: string; hw_id: string };
  icds: { key: string; id: string; version: string; title: string; messages: number }[];
  traceability: { summary: TraceSummary | null; requirements: Requirement[]; areas: { name: string; requirements: string[] }[] };
  bench: { nominal: BenchRun | null; campaign: BenchRun | null };
  heldout: Record<string, HeldoutSet>;
  agents: { summary: Record<string, AgentModel>; drafts: AgentDraft[] } | null;
  assurance: Record<string, Record<string, unknown> | null>;
  sbom: { name: string; version: string; scope: string }[];
  release: { manifest: Record<string, unknown> | null; build_signature: Record<string, unknown> | null };
}

export function isConsole(x: unknown): x is Console {
  if (typeof x !== "object" || x === null) return false;
  const r = x as Record<string, unknown>;
  return typeof r.generated_at === "string" && typeof r.traceability === "object" && r.traceability !== null;
}
