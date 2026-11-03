import { useCallback, useEffect, useState } from "react";
import { type Console, isConsole } from "./console";

export type Load = { kind: "loading" } | { kind: "error"; message: string } | { kind: "empty" } | { kind: "ready"; data: Console };

/** Loads the generated console data; distinguishes a missing report (empty) from a failure (error). */
export function useConsole(url = "data/console.json"): { state: Load; retry: () => void } {
  const [state, setState] = useState<Load>({ kind: "loading" });
  const [attempt, setAttempt] = useState(0);
  const retry = useCallback(() => setAttempt((a) => a + 1), []);
  // biome-ignore lint/correctness/useExhaustiveDependencies: attempt re-runs the load
  useEffect(() => {
    let live = true;
    setState({ kind: "loading" });
    fetch(url, { cache: "no-store" })
      .then(async (r) => {
        if (r.status === 404) return { kind: "empty" } as const;
        if (!r.ok) throw new Error(`The report could not be loaded (HTTP ${r.status}).`);
        const body: unknown = await r.json().catch(() => {
          throw new Error("The report is not valid JSON.");
        });
        if (!isConsole(body)) throw new Error("The file is not an evidence report. Regenerate it with `iqk report`.");
        return { kind: "ready", data: body } as const;
      })
      .then((s) => live && setState(s))
      .catch((e: Error) => live && setState({ kind: "error", message: e.message }));
    return () => {
      live = false;
    };
  }, [url, attempt]);
  return { state, retry };
}
