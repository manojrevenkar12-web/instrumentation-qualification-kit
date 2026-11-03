import { AnimatePresence, motion } from "motion/react";
import type { Requirement } from "../../shared/lib/console";
import { statusLabel, statusTone, testName } from "../../shared/lib/status";
import { motionTokens, toneVar } from "../../shared/theme/tokens";
import s from "./proof.module.css";

const PROOF: Record<string, string> = {
  proven: "fails on a mutant",
  unproven: "catches none of the mutants",
  system: "system level",
};

export function RequirementDetail({ r }: { r: Requirement }) {
  const tone = toneVar[statusTone(r.status)];
  return (
    <AnimatePresence mode="wait">
      <motion.article
        key={r.id}
        className={s.detail}
        aria-live="polite"
        initial={{ opacity: 0 }}
        animate={{ opacity: 1 }}
        exit={{ opacity: 0 }}
        transition={motionTokens.quick}
      >
        <div>
          <h3 className={s.dTitle}>
            <span className={s.dId}>{r.id}</span> {r.title}
          </h3>
          <p className={s.dText}>{r.text}</p>
          <span className={s.badge} style={{ color: tone.fg, background: tone.bg }}>
            {statusLabel(r.status)}
          </span>
          <p className={s.meta}>
            Verified by {r.method}
            {r.cites.length ? `; depends on ${r.cites.join(", ")}` : ""}
            {r.evidence ? `; evidence in ${r.evidence}` : ""}.
          </p>
        </div>
        <div>
          {r.links.length ? (
            <>
              <p className={s.listTitle}>Tests that claim it</p>
              <ul className={s.list}>
                {r.links.map((l) => (
                  <li key={l.test}>
                    <span>{testName(l.test)}</span>
                    <span className={s.small}>
                      {l.passed ? "passes" : "FAILS"}, {PROOF[l.proof]}
                      {l.kills.length ? `: ${l.kills.join(", ")}` : ""}
                    </span>
                  </li>
                ))}
              </ul>
            </>
          ) : null}
          {r.mutants.length ? (
            <>
              <p className={s.listTitle} style={{ marginTop: "var(--s-4)" }}>
                Mutants: how this requirement could be broken
              </p>
              <ul className={s.list}>
                {r.mutants.map((m) => (
                  <li key={m.name}>
                    <span>{m.name.replaceAll("_", " ")}</span>
                    <span className={s.small}>
                      {m.caught_by.length ? `caught by ${m.caught_by.map(testName).join(", ")}` : `not caught (${m.outcome})`}
                    </span>
                  </li>
                ))}
              </ul>
            </>
          ) : null}
        </div>
      </motion.article>
    </AnimatePresence>
  );
}
