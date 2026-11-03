// Motion tokens and the status vocabulary shared by every feature.
export const motionTokens = {
  reveal: { duration: 0.5, ease: [0.2, 0.7, 0.2, 1] as [number, number, number, number] },
  stagger: 0.012,
  quick: { duration: 0.18, ease: "easeOut" as const },
} as const;

export type Tone = "proven" | "weak" | "failed" | "system" | "neutral";

export const toneVar: Record<Tone, { fg: string; bg: string }> = {
  proven: { fg: "var(--proven)", bg: "var(--proven-soft)" },
  weak: { fg: "var(--weak)", bg: "var(--weak-soft)" },
  failed: { fg: "var(--failed)", bg: "var(--failed-soft)" },
  system: { fg: "var(--system)", bg: "var(--system-soft)" },
  neutral: { fg: "var(--ink-3)", bg: "var(--rule)" },
};
