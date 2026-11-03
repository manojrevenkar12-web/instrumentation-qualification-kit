import { motion } from "motion/react";
import type { BenchRun } from "../../shared/lib/console";
import { us } from "../../shared/lib/status";
import { motionTokens } from "../../shared/theme/tokens";
import p from "../../shared/ui/panels.module.css";
import { EmptyState } from "../../shared/ui/states";

const LIMIT_US = 1000;

function label(lo: number, hi: number | undefined): string {
  return hi === undefined ? `≥ ${us(lo)}` : `${us(lo)}–${us(hi)}`;
}

function Histogram({ run, title }: { run: BenchRun; title: string }) {
  const { edges_us, counts } = run.time_error_histogram;
  const total = counts.reduce((a, b) => a + b, 0) || 1;
  const max = Math.max(...counts, 1);
  return (
    <figure style={{ margin: 0 }} aria-labelledby={`${run.name}-cap`}>
      <p className={p.subhead}>{title}</p>
      {counts.map((c, i) => {
        const over = edges_us[i] >= LIMIT_US;
        return (
          <div className={p.hbar} key={edges_us[i]}>
            <span>{label(edges_us[i], edges_us[i + 1])}</span>
            <span className={p.track} aria-hidden="true">
              <motion.span
                style={{ background: over ? "var(--failed)" : "var(--proven)", height: "100%" }}
                initial={{ width: 0 }}
                whileInView={{ width: `${(100 * c) / max}%` }}
                viewport={{ once: true }}
                transition={motionTokens.reveal}
              />
            </span>
            <span className={p.num}>{((100 * c) / total).toFixed(c && (100 * c) / total < 0.1 ? 2 : 1)}%</span>
          </div>
        );
      })}
      <figcaption className={p.caption} id={`${run.name}-cap`}>
        {run.time_error_us.n.toLocaleString("en")} time-coded samples, stamped while locked. Median {us(run.time_error_us.p50)}, 99th
        percentile {us(run.time_error_us.p99)}, worst {us(run.time_error_us.max)}. Bars at or beyond the 1 ms requirement are red.
      </figcaption>
    </figure>
  );
}

export function TimeAccuracy({ nominal, campaign }: { nominal: BenchRun | null; campaign: BenchRun | null }) {
  if (!nominal && !campaign) {
    return (
      <EmptyState title="No bench runs yet">
        Run the daemon against the emulators with <code>iqk bench --seconds 30 --report nominal</code>.
      </EmptyState>
    );
  }
  return (
    <>
      <div className={p.stats}>
        {nominal ? (
          <>
            <div className={p.stat}>
              <span className={p.statValue}>{us(nominal.time_error_us.p99)}</span>
              <span className={p.statLabel}>99th percentile time-stamp error, PTP locked (limit 1 ms)</span>
            </div>
            <div className={p.stat}>
              <span className={p.statValue}>{nominal.packets.toLocaleString("en")}</span>
              <span className={p.statLabel}>
                telemetry packets in {nominal.seconds} s, {nominal.crc_errors_at_receiver} with a bad CRC
              </span>
            </div>
          </>
        ) : null}
        {campaign ? (
          <div className={p.stat}>
            <span className={p.statValue}>{us(campaign.time_error_us.p99)}</span>
            <span className={p.statLabel}>99th percentile under the fault campaign, through a PTP outage</span>
          </div>
        ) : null}
      </div>
      <div className={p.grid2}>
        {nominal ? <Histogram run={nominal} title="Nominal run" /> : null}
        {campaign ? <Histogram run={campaign} title="Under faults" /> : null}
      </div>
    </>
  );
}
