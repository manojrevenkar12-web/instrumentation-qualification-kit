import s from "./ui.module.css";

export function Skeleton({ lines = 4, label }: { lines?: number; label: string }) {
  return (
    <div className={s.skeleton} role="status" aria-live="polite">
      <span className="visually-hidden">{label}</span>
      {Array.from({ length: lines }, (_, i) => (
        // biome-ignore lint/suspicious/noArrayIndexKey: placeholder bars have no identity
        <div key={i} className={s.bar} style={{ width: `${100 - i * 12}%` }} />
      ))}
    </div>
  );
}

export function ErrorState({ message, onRetry }: { message: string; onRetry: () => void }) {
  return (
    <div className={s.noteError} role="alert">
      <p className={s.noteTitle}>The evidence could not be loaded.</p>
      <p className={s.noteBody}>{message}</p>
      <button type="button" className={s.button} onClick={onRetry}>
        Load again
      </button>
    </div>
  );
}

export function EmptyState({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className={s.noteEmpty}>
      <p className={s.noteTitle}>{title}</p>
      <p className={s.noteBody}>{children}</p>
    </div>
  );
}

export function Cmd({ children }: { children: string }) {
  return <code className={s.code}>{children}</code>;
}
