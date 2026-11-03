# Writing a held-out conformance set

A held-out set checks the unit against its documents, written by someone who has **not** read the unit's code,
tests, mutants or reports. In qualification terms it is independent verification: if the writer and the
implementer read the documents differently, the set finds it.

## What you may read

- `docs/icd/*.md` (the interface control documents)
- `docs/PLAN_FORMAT.md` (the measurement plan format and its rules)
- this file

Do not open anything under `cpp/`, `iqk/`, `tests/`, `qual/`, `reports/`, `requirements/` or other held-out
sets, and do not run the unit's tools. Work only from the documents.

## What to write

Put your files in `heldout/<set>/` (the set name you are given).

### 1. Codec vectors: `vectors.yaml`

```yaml
cases:
  - name: imu sample, all fields set            # short, unique
    icd: imu                                   # the file stem of docs/icd/<icd>.md
    message: imu_sample                        # a message name from that ICD
    payload_hex: "0201..."                     # the payload only (no framing, no CRC trailer)
    expect: decode                             # decode or reject
    fields: {seq: 258, t_us: 168496141, ...}   # raw integer values, every field, when expect is decode
```

- Write the bytes yourself from the ICD tables (offsets, sizes, byte order, two's complement for signed).
- Arrays are given as `samples: [{channel: 1, dt_us: 0, value: -5, flags: 0}, ...]`.
- `reject` cases: a payload one byte too short or too long, an array whose count disagrees with the length,
  more array records than the ICD allows.
- Cover every ICD, both byte orders, signed extremes, the 48-bit field, and arrays with 0, 1 and the maximum
  number of records. Aim for 30 to 50 cases.

### 2. Plans: `plans/*.plan`

Each plan starts with an expectation comment:

```
# expect: accept
```
or
```
# expect: reject PLN-06 PLN-10
```

listing every rule the plan breaks. Aim for 25 to 40 plans: boundaries (exactly 60 % bus load and just above,
exactly 48 channels and 49, alarm limits exactly at the measurable range and just beyond, negative scale),
several rules broken at once, and plans that look wrong but are valid. Compute bus load and telemetry rate by
hand from the formulas in PLAN_FORMAT.md, and note your arithmetic in a comment.

### 3. `README.md`

Who wrote the set, from which documents, and anything in the documents you found ambiguous. Ambiguities are
findings: list them rather than guessing silently.

## Rules for the set once written

The set runs once (`iqk heldout run <set>`); its first run is written to `reports/heldout/<set>/first_run.json`
and is never rewritten. If anyone studies its failures to change the unit, the set becomes a regression set
(`heldout/<set>/STATUS` = `regression`) and a new held-out set is written from these documents by someone who
has not seen the code.
