import type { BenchRun } from "../../shared/lib/console";
import p from "../../shared/ui/panels.module.css";
import { EmptyState } from "../../shared/ui/states";

const NAMES: Record<string, string> = {
  imu_bit_errors: "Bit errors on the IMU line",
  imu_garbage: "Garbage bytes before IMU frames",
  imu_drop: "IMU frames dropped",
  imu_silent: "IMU silent for 200 ms",
  imu_out_of_range: "Impossible IMU values",
  aam_silent: "Module 2 stops (heartbeat and data)",
  ptp_jitter: "PTP Sync messages held up 15 ms",
  ptp_outage: "PTP grandmaster gone",
};

export function FaultCampaign({ run }: { run: BenchRun | null }) {
  if (!run) {
    return (
      <EmptyState title="No fault campaign recorded">
        Run it with <code>iqk bench --seconds 24 --report campaign</code> and the faults of the system test.
      </EmptyState>
    );
  }
  const span = run.seconds || 1;
  const logged = Object.entries(run.faults_logged).sort((a, b) => b[1] - a[1]);
  return (
    <div className={p.grid2}>
      <figure style={{ margin: 0 }}>
        <p className={p.subhead}>Injected, over {Math.round(span)} s</p>
        {run.faults_injected.map((f) => {
          const to = f.to_s ?? span;
          return (
            <div className={p.hbar} key={f.name} style={{ gridTemplateColumns: "minmax(0, 13rem) minmax(0, 1fr)" }}>
              <span>{NAMES[f.name] ?? f.name}</span>
              <span className={p.track} role="img" aria-label={`from ${f.from_s} s to ${to} s`}>
                <span style={{ width: `${(100 * f.from_s) / span}%` }} />
                <span style={{ width: `${(100 * (Math.min(to, span) - f.from_s)) / span}%`, background: "var(--weak)" }} />
              </span>
            </div>
          );
        })}
        <figcaption className={p.caption}>
          The unit kept streaming throughout: {run.packets.toLocaleString("en")} packets, {run.crc_errors_at_receiver} with a bad CRC at the
          receiver.
        </figcaption>
      </figure>
      <div>
        <p className={p.subhead}>What the unit reported</p>
        <div className={p.tableWrap}>
          <table className={p.table}>
            <thead>
              <tr>
                <th scope="col">Fault in the unit's log</th>
                <th scope="col" className={p.num}>
                  Count
                </th>
              </tr>
            </thead>
            <tbody>
              {logged.map(([k, v]) => (
                <tr key={k}>
                  <td>{k}</td>
                  <td className={p.num}>{v}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>
    </div>
  );
}
