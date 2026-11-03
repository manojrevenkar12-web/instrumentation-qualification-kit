// Copies the generated console data into the app (reports/console.json is the single source).
import { copyFileSync, existsSync, mkdirSync } from "node:fs";
const src = new URL("../../reports/console.json", import.meta.url);
mkdirSync(new URL("../public/data/", import.meta.url), { recursive: true });
if (existsSync(src)) copyFileSync(src, new URL("../public/data/console.json", import.meta.url));
else console.warn("reports/console.json not found; run `python3 -m iqk.cli report` first");
