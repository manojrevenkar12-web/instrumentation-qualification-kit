import s from "./ui.module.css";

export function Section({ id, title, lede, children }: { id: string; title: string; lede?: string; children: React.ReactNode }) {
  return (
    <section className={s.section} aria-labelledby={`${id}-title`} id={id}>
      <div className={s.head}>
        <h2 className={s.title} id={`${id}-title`}>
          {title}
        </h2>
        {lede ? <p className={s.lede}>{lede}</p> : null}
      </div>
      {children}
    </section>
  );
}
