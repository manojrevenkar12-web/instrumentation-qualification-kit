import type { Console } from "../../shared/lib/console";
import p from "../../shared/ui/panels.module.css";
import { EmptyState } from "../../shared/ui/states";

const PARTS = [
  { key: "proven", label: "Proven: catches every mutant", color: "var(--proven)" },
  { key: "partly_proven", label: "Partly: catches some", color: "var(--weak)" },
  { key: "hollow", label: "Hollow: passes, catches none", color: "var(--failed)" },
  { key: "unusable", label: "Did not compile or pass", color: "var(--rule-strong)" },
] as const;

export function Agents({ agents }: { agents: Console["agents"] }) {
  if (!agents || !Object.keys(agents.summary).length) {
    return (
      <EmptyState title="No agent drafts measured yet">
        Run <code>iqk agents</code>: Claude Code drafts tests per requirement in a sandbox, and mutation decides what they are worth.
      </EmptyState>
    );
  }
  const models = Object.entries(agents.summary);
  return (
    <>
      {models.map(([model, m]) => {
        const parts = { proven: m.proven, partly_proven: m.partly_proven, hollow: m.hollow, unusable: m.drafts - m.passed };
        return (
          <div key={model} className={p.hbar} style={{ gridTemplateColumns: "5rem minmax(0, 1fr) 9rem" }}>
            <span style={{ textTransform: "capitalize", fontWeight: "var(--w-medium)" }}>{model}</span>
            <span
              className={p.track}
              role="img"
              aria-label={`${model}: ${m.proven} proven, ${m.partly_proven} partly proven, ${m.hollow} hollow, ${m.drafts - m.passed} unusable of ${m.drafts} drafts`}
            >
              {PARTS.map((pt) => (
                <span key={pt.key} style={{ width: `${(100 * parts[pt.key]) / Math.max(m.drafts, 1)}%`, background: pt.color }} />
              ))}
            </span>
            <span className={p.num}>
              {m.proven} of {m.drafts}, ${m.cost_usd.toFixed(2)}
            </span>
          </div>
        );
      })}
      <ul
        style={{ display: "flex", flexWrap: "wrap", gap: "var(--s-2) var(--s-5)", listStyle: "none", padding: 0, margin: "var(--s-4) 0 0" }}
      >
        {PARTS.map((pt) => (
          <li
            key={pt.key}
            style={{ display: "flex", gap: "var(--s-2)", alignItems: "center", fontSize: "var(--fs-s)", color: "var(--ink-2)" }}
          >
            <span style={{ width: "var(--s-4)", height: "var(--s-4)", borderRadius: "var(--radius-s)", background: pt.color }} />
            {pt.label}
          </li>
        ))}
      </ul>
      <p className={p.caption}>
        Each draft was written from the requirement text, the headers and the ICDs, in a workspace without the project's tests or mutants,
        then run against the requirement's mutants. Drafts are proposals; an engineer reviews and signs what enters the evidence.
      </p>
    </>
  );
}
