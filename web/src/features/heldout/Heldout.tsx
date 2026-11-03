import type { HeldoutSet } from "../../shared/lib/console";
import p from "../../shared/ui/panels.module.css";
import { EmptyState } from "../../shared/ui/states";

export function Heldout({ sets }: { sets: Record<string, HeldoutSet> }) {
  const entries = Object.entries(sets);
  if (!entries.length) {
    return (
      <EmptyState title="No held-out set has run">
        Have someone who has not read the code write one from <code>heldout/WRITING_VECTORS.md</code>, then run it once.
      </EmptyState>
    );
  }
  return (
    <>
      {entries.map(([name, s]) => (
        <div key={name}>
          <div className={p.stats}>
            <div className={p.stat}>
              <span className={p.statValue}>
                {s.summary.vectors_pass} / {s.summary.vectors}
              </span>
              <span className={p.statLabel}>wire-format vectors decoded or rejected as the documents say</span>
            </div>
            <div className={p.stat}>
              <span className={p.statValue}>
                {s.summary.plans_pass} / {s.summary.plans}
              </span>
              <span className={p.statLabel}>measurement plans accepted or refused with the expected rules</span>
            </div>
            <div className={p.stat}>
              <span className={p.statValue}>{s.status === "heldout" ? "Unstudied" : "Regression"}</span>
              <span className={p.statLabel}>
                first run of {name}, {s.summary.recorded_at.slice(0, 10)}, kept as recorded
              </span>
            </div>
          </div>
          {s.failed.length ? (
            <div className={p.tableWrap}>
              <table className={p.table}>
                <caption className="visually-hidden">Disagreements in the first run</caption>
                <thead>
                  <tr>
                    <th scope="col">Where the set and the unit disagree</th>
                    <th scope="col">What happened</th>
                  </tr>
                </thead>
                <tbody>
                  {s.failed.map((f) => (
                    <tr key={f.name}>
                      <td>{f.name.replace(/\.plan$/, "").replaceAll("_", " ")}</td>
                      <td>{f.detail}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          ) : null}
        </div>
      ))}
    </>
  );
}
