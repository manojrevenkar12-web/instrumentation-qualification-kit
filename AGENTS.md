# Working on this repository (people and coding agents)

## Fast feedback

```bash
scripts/check.sh            # generated files current, build, 80+ unit tests, lint, Python tests (about a minute)
scripts/check.sh --system   # plus the SITL bench: the real daemon against the emulators (two minutes more)
python3 -m iqk.cli qual mutate --only SRS-0xx   # prove the links of the requirements you touched
python3 -m iqk.cli qual matrix                  # traceability and the release gate
```

## Invariants (each has a test or a gate)

- **The ICDs are the single source.** `icd/*.yaml` generates the C++ codecs, the dispatch table and
  `docs/icd/*.md`. Never edit `cpp/gen/`; run `iqk icd generate`. CI fails on drift (`iqk icd check`).
- **Settings live in `config/unit.yaml`**, generated into `cpp/gen/iqk/config.hpp`. No thresholds in code.
  Requirements cite the values they depend on, so `iqk qual impact` can say what a change re-opens.
- **The core is deterministic.** `iqk::Dau` and everything under it take the local time as an input and never
  read a clock, start a thread or allocate on the acquisition path. `daud` is the only code that touches
  sockets, serial ports and the clock. Replay must stay byte-exact (SRS-100).
- **A plan reaches the hardware only after validation, read-back and the module CRC** (SRS-040..055).
  Telemetry never flows under an unverified plan.
- **A link is evidence only when mutation proves it.** A test declares what it verifies (`IQK_VERIFIES`,
  `@pytest.mark.verifies`); `qual/mutants.yaml` holds each requirement's plausible violations. A new
  requirement comes with mutants; a mutant that survives means the tests are weak, never that the mutant is
  inconvenient. Delete a mutant only when it is equivalent, and say why in the commit.
- **Warnings are errors**, clang-tidy and cppcheck are clean, and the unit tests pass under ASan and UBSan.
- **Agents draft, engineers sign.** Coding agents may write tests, emulators, interface code and evidence
  drafts. They hold no signing key. A release is qualified only when an engineer signs its manifest.

## Held-out sets

`heldout/<set>` is written from `heldout/WRITING_VECTORS.md`, `docs/icd/` and `docs/PLAN_FORMAT.md` only, by
someone who has not read the code. It runs once (`iqk heldout run <set>`); `reports/heldout/<set>/first_run.json`
is never rewritten. Never change the unit to pass a held-out set. If you study its failures, write `regression`
to `heldout/<set>/STATUS` and have someone who has not seen the code write a new set.

## Where things are

| Path | What |
|---|---|
| `icd/` | interface control documents (YAML), the source of `cpp/gen/` and `docs/icd/` |
| `config/unit.yaml` | unit settings (time sync, links, plan limits, update) |
| `cpp/include`, `cpp/src` | the unit: framing, drivers, time sync, plan validation and loading, telemetry, diagnostics, secure update |
| `cpp/apps` | `daud` (the daemon) and `iqkctl` (validate plans, codec, replay, packages, update) |
| `cpp/tests` | unit tests (GoogleTest) with requirement tags |
| `iqk/bench.py` | SITL emulators (IMU, GNSS + PPS, acquisition modules, PTP grandmaster, telemetry recorder) |
| `requirements/srs.yaml` | software requirements |
| `qual/mutants.yaml` | mutants per requirement |
| `iqk/qual/` | evidence, mutation proof, matrix, change impact, release bundle |
| `iqk/assurance/` | reproducible build, hardening, static analysis, sanitizers, SBOM |
| `iqk/agents/` | the agent-drafted test experiment |
| `reports/` | generated; do not edit by hand |
