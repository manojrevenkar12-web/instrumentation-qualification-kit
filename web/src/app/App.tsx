import { useState } from "react";
import { Agents } from "../features/agents/Agents";
import { FaultCampaign } from "../features/bench/FaultCampaign";
import { TimeAccuracy } from "../features/bench/TimeAccuracy";
import { Heldout } from "../features/heldout/Heldout";
import { ProofStrip } from "../features/proof/ProofStrip";
import { RequirementDetail } from "../features/proof/RequirementDetail";
import { Supply } from "../features/supply/Supply";
import { useConsole } from "../shared/lib/useConsole";
import { Section } from "../shared/ui/Section";
import { Cmd, EmptyState, ErrorState, Skeleton } from "../shared/ui/states";
import s from "./app.module.css";

const NAV = [
  ["proof", "Proof"],
  ["time", "Time"],
  ["faults", "Faults"],
  ["blind", "Written blind"],
  ["agents", "Agents"],
  ["ships", "What ships"],
] as const;

export function App({ url }: { url?: string }) {
  const { state, retry } = useConsole(url);
  const [selected, setSelected] = useState<string | null>(null);

  return (
    <div className={s.page}>
      <header className={s.top}>
        <span className={s.brand}>Data acquisition unit, qualification evidence</span>
        <nav aria-label="Sections" className={s.nav}>
          {NAV.map(([id, label]) => (
            <a key={id} href={`#${id}`}>
              {label}
            </a>
          ))}
        </nav>
      </header>
      <main id="proof">
        {state.kind === "loading" ? (
          <div style={{ paddingBlock: "var(--s-7)" }}>
            <Skeleton lines={6} label="Loading the evidence" />
          </div>
        ) : state.kind === "error" ? (
          <div style={{ paddingBlock: "var(--s-7)" }}>
            <ErrorState message={state.message} onRetry={retry} />
          </div>
        ) : state.kind === "empty" ? (
          <div style={{ paddingBlock: "var(--s-7)" }}>
            <EmptyState title="No evidence has been generated for this build">
              Run the qualification pipeline, then <Cmd>python3 -m iqk.cli report</Cmd> and reload.
            </EmptyState>
          </div>
        ) : (
          (() => {
            const c = state.data;
            const reqs = c.traceability.requirements;
            const current = reqs.find((r) => r.id === selected) ?? reqs.find((r) => r.status !== "verified") ?? reqs[0];
            return (
              <>
                {reqs.length ? (
                  <>
                    <ProofStrip c={c} selected={current?.id ?? null} onSelect={setSelected} />
                    {current ? <RequirementDetail r={current} /> : null}
                  </>
                ) : (
                  <div style={{ paddingBlock: "var(--s-7)" }}>
                    <EmptyState title="The traceability matrix is empty">
                      Generate it with <Cmd>python3 -m iqk.cli qual matrix</Cmd>.
                    </EmptyState>
                  </div>
                )}
                <Section
                  id="time"
                  title="Time stamps against true time"
                  lede="One input of an emulated module carries a time code from the bench's true clock, so every stamp the unit puts on a sample can be checked end to end: serial and CAN links, PTP or GNSS, servo and telemetry."
                >
                  <TimeAccuracy nominal={c.bench.nominal} campaign={c.bench.campaign} />
                </Section>
                <Section
                  id="faults"
                  title="The fault campaign"
                  lede="The same daemon, on the same links, while the emulators misbehave on a schedule."
                >
                  <FaultCampaign run={c.bench.campaign} />
                </Section>
                <Section
                  id="blind"
                  title="Written blind, run once"
                  lede="Wire-format vectors and measurement plans written only from the interface documents, by a session that never saw the code. The first run is kept as recorded."
                >
                  <Heldout sets={c.heldout} />
                </Section>
                <Section
                  id="agents"
                  title="Coding agents as test drafters"
                  lede="Agent-drafted tests measured exactly like an engineer's: do they compile, pass, and fail when the requirement is broken?"
                >
                  <Agents agents={c.agents} />
                </Section>
                <Section
                  id="ships"
                  title="What ships, and how it was built"
                  lede={`Software ${c.unit.sw_version} for hardware ${c.unit.hw_id}.`}
                >
                  <Supply c={c} />
                  <p style={{ margin: "var(--s-5) 0 var(--s-2)", fontSize: "var(--fs-s)", color: "var(--ink-2)" }}>
                    Interface documents in this release
                  </p>
                  <ul className={s.icds}>
                    {c.icds.map((i) => (
                      <li key={i.key}>
                        {i.id} {i.version}
                      </li>
                    ))}
                  </ul>
                </Section>
                <footer className={s.foot}>
                  Generated {c.generated_at.replace("T", " ").replace("+00:00", " UTC")} from the reports of this build.
                </footer>
              </>
            );
          })()
        )}
      </main>
    </div>
  );
}
