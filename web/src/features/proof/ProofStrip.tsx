import { motion } from "motion/react";
import type { Console, Requirement } from "../../shared/lib/console";
import { caught, statusLabel, statusTone } from "../../shared/lib/status";
import { motionTokens, toneVar } from "../../shared/theme/tokens";
import s from "./proof.module.css";

const LEGEND: { tone: keyof typeof toneVar; label: string }[] = [
  { tone: "proven", label: "Verified, every mutant caught" },
  { tone: "system", label: "Verified on the bench or by analysis" },
  { tone: "weak", label: "Weak or not yet run" },
  { tone: "failed", label: "Failing or untested" },
];

function verdict(c: Console): { headline: string; sub: string } {
  const t = c.traceability.summary;
  const v = c.unit.sw_version;
  if (!t) return { headline: `Release ${v} has no traceability yet.`, sub: "Run the qualification pipeline to produce it." };
  const ok = t.by_status.verified ?? 0;
  const sys = Object.entries(t.by_status)
    .filter(([k]) => k !== "verified" && k.startsWith("verified"))
    .reduce((n, [, x]) => n + x, 0);
  const rest = t.requirements - ok - sys;
  const signed = Boolean(c.release.build_signature);
  const headline = t.gate.pass
    ? `Release ${v} meets every requirement and waits for an engineer's signature.`
    : `Release ${v} cannot ship yet: ${rest} of ${t.requirements} requirements are not verified.`;
  const sub =
    `${ok} requirements are verified by tests that fail on every one of their ${t.mutants_caught_by_linked_tests} mutants, ` +
    `${sys} more on the bench or by analysis. ${t.tests_passed} of ${t.tests} tests pass` +
    (signed ? "; the pipeline has signed the release manifest." : ".");
  return { headline, sub };
}

export function ProofStrip({ c, selected, onSelect }: { c: Console; selected: string | null; onSelect: (id: string) => void }) {
  const byId = new Map(c.traceability.requirements.map((r) => [r.id, r]));
  const { headline, sub } = verdict(c);
  const gate = c.traceability.summary?.gate;
  let order = 0;
  return (
    <div className={s.hero}>
      <h1 className={s.eyebrowless}>{headline}</h1>
      <p className={s.sub}>{sub}</p>
      {gate ? (
        <ul className={s.gate} aria-label="Release gate">
          {(
            [
              ["all_requirements_verified", "All requirements verified"],
              ["every_mutant_caught", "Every mutant caught"],
              ["no_failing_tests", "No failing test"],
              ["generated_files_current", "Generated code matches the ICDs"],
            ] as const
          ).map(([k, label]) => (
            <li key={k} style={{ color: gate[k] ? toneVar.proven.fg : toneVar.failed.fg }}>
              {gate[k] ? "✓" : "✗"} {label}
            </li>
          ))}
        </ul>
      ) : null}
      <fieldset className={s.strip}>
        <legend className="visually-hidden">Requirements by subsystem; choose one to see its evidence</legend>
        {c.traceability.areas.map((a) => (
          <div key={a.name} className={s.area}>
            <div className={s.areaName}>{a.name}</div>
            {a.requirements.map((id) => {
              const r = byId.get(id) as Requirement;
              const tone = toneVar[statusTone(r.status)];
              const i = order++;
              return (
                <motion.button
                  type="button"
                  key={id}
                  className={s.tile}
                  aria-pressed={selected === id}
                  aria-label={`${id} ${r.title}: ${statusLabel(r.status)}${r.mutants.length ? `, ${caught(r)} of ${r.mutants.length} mutants caught` : ""}`}
                  title={`${id} ${r.title}`}
                  onClick={() => onSelect(id)}
                  style={{ background: tone.bg }}
                  initial={{ opacity: 0, y: 6 }}
                  animate={{ opacity: 1, y: 0 }}
                  transition={{ ...motionTokens.reveal, delay: i * motionTokens.stagger }}
                >
                  <span className={s.num}>{id.slice(4)}</span>
                  <span className={s.ticks} aria-hidden="true" style={{ color: tone.fg }}>
                    {r.mutants.map((m) => (
                      <span key={m.name} className={s.tick} style={{ background: m.caught_by.length ? tone.fg : "transparent" }} />
                    ))}
                  </span>
                </motion.button>
              );
            })}
          </div>
        ))}
      </fieldset>
      <ul className={s.legend}>
        {LEGEND.map((l) => (
          <li key={l.tone}>
            <span className={s.swatch} style={{ background: toneVar[l.tone].bg, border: `1px solid ${toneVar[l.tone].fg}` }} />
            {l.label}
          </li>
        ))}
        <li>
          <span className={s.ticks} aria-hidden="true">
            <span className={s.tick} style={{ background: "currentColor" }} />
            <span className={s.tick} />
          </span>
          A mutant: filled when a linked test catches it
        </li>
      </ul>
    </div>
  );
}
