import type { Console } from "../../shared/lib/console";
import p from "../../shared/ui/panels.module.css";
import { EmptyState } from "../../shared/ui/states";

type Rec = Record<string, unknown>;

const HARDENING: Record<string, string> = {
  pie: "PIE",
  full_relro: "full RELRO",
  nx_stack: "NX stack",
  stack_protector: "stack protector",
  fortify: "FORTIFY",
  cet_ibt_shstk: "CET (IBT, shadow stack)",
};

function Check({ ok, label, detail }: { ok: boolean | undefined; label: string; detail: string }) {
  return (
    <tr>
      <td>{label}</td>
      <td className={ok === undefined ? undefined : ok ? p.yes : p.no}>{ok === undefined ? "not run" : ok ? "pass" : "fail"}</td>
      <td>{detail}</td>
    </tr>
  );
}

export function Supply({ c }: { c: Console }) {
  const a = c.assurance;
  const hard = a.hardening as Rec | null;
  const bins = (hard?.binaries ?? {}) as Record<string, Record<string, boolean>>;
  const repro = a.repro as Rec | null;
  const stat = a.static as Rec | null;
  const sig = c.release.build_signature as Rec | null;
  const rel = a.release as Rec | null;
  if (!hard && !repro && !stat && !c.sbom.length) {
    return (
      <EmptyState title="No assurance reports yet">
        Run the qualification job, or <code>iqk assurance hardening</code> and the other checks.
      </EmptyState>
    );
  }
  const flags = Object.values(bins)[0] ?? {};
  return (
    <div className={p.grid2}>
      <div>
        <p className={p.subhead}>Checks on what ships</p>
        <div className={p.tableWrap}>
          <table className={p.table}>
            <thead>
              <tr>
                <th scope="col">Check</th>
                <th scope="col">Result</th>
                <th scope="col">Evidence</th>
              </tr>
            </thead>
            <tbody>
              <Check
                ok={repro ? Boolean(repro.pass) : undefined}
                label="Reproducible build"
                detail="two builds in different directories, identical binaries"
              />
              <Check
                ok={hard ? Boolean(hard.pass) : undefined}
                label="Hardened binaries"
                detail={
                  Object.keys(flags)
                    .map((k) => HARDENING[k] ?? k)
                    .join(", ") || "PIE, full RELRO, NX stack, stack protector, FORTIFY, CET"
                }
              />
              <Check
                ok={stat ? Boolean(stat.pass) : undefined}
                label="Static analysis"
                detail={
                  stat ? `clang-tidy ${stat.clang_tidy_findings} findings, cppcheck ${stat.cppcheck_findings}` : "clang-tidy and cppcheck"
                }
              />
              <Check
                ok={a.sanitize ? Boolean((a.sanitize as Rec).pass) : undefined}
                label="Sanitizers"
                detail="unit tests under ASan and UBSan"
              />
              <Check
                ok={rel ? Boolean(rel.pass) : undefined}
                label="Release bundle"
                detail={
                  sig ? `manifest signed by the pipeline; ${String(rel?.engineer_approval ?? "awaiting engineer sign-off")}` : "not built"
                }
              />
            </tbody>
          </table>
        </div>
      </div>
      <div>
        <p className={p.subhead}>Software bill of materials</p>
        <div className={p.tableWrap}>
          <table className={p.table}>
            <thead>
              <tr>
                <th scope="col">Component</th>
                <th scope="col">Version</th>
                <th scope="col">Ships</th>
              </tr>
            </thead>
            <tbody>
              {c.sbom.map((x) => (
                <tr key={x.name}>
                  <td>{x.name}</td>
                  <td>{x.version}</td>
                  <td>{x.scope === "excluded" ? "no (tools, tests)" : "yes"}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>
    </div>
  );
}
