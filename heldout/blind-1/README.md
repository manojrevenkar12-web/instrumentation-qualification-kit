# Held-out conformance set `blind-1`

## Who wrote it

An independent Claude session given only the documents, on 2026-11-03. The writer did not open, list,
search or run anything else in the repository (no `cpp/`, `iqk/`, `tests/`, `qual/`, `reports/`,
`requirements/`, `config/`, `icd/*.yaml`, `build/`, `examples/`, other held-out sets or git history) and did
not run the unit's tools.

## Documents used

- `heldout/WRITING_VECTORS.md`
- `docs/PLAN_FORMAT.md`
- `docs/icd/aam.md` (AAM-CAN 2.0.1), `docs/icd/gnss.md` (GNSS-SER 1.0.3), `docs/icd/imu.md` (IMU-SER 1.2.0),
  `docs/icd/ptp.md` (PTP-UDP 1.1.0), `docs/icd/tlm.md` (DAU-UDP 1.3.0)

## Contents

- `vectors.yaml`: 50 codec cases (29 decode, 21 reject) covering every message of all five ICDs. PTP is
  the only big-endian ICD. The cases include signed extremes (i8, i16, i32, i64), the 48-bit `origin_s`
  field (distinct bytes and the u48 maximum), u64 maxima, unaligned fields, arrays with 0, 1, 3 and the
  maximum number of records (48 samples, 32 fault records), and rejects for one byte too short or too
  long, a count that disagrees with the length, and one record more than allowed.
- `plans/*.plan`: 40 plans (13 accept, 27 reject; 3 of the rejects break several rules). Each plan starts
  with `# expect:`. Plans that have channels show the CAN-load and telemetry arithmetic in comments.

How it was checked: the payload bytes were packed by hand from the ICD tables in a scratch directory
outside the repository. A second decoder, written separately with Python `struct` format strings, decoded
every `decode` case back to the listed fields. Each plan's expectation was set by hand and then compared
with a small checker the writer built from PLAN_FORMAT.md. The two agreed on all 40 plans.

### Boundaries worth knowing

- **Exactly 60 % CAN load cannot be reached.** Load = 95·M + 135·R bit/s (M = modules used, R = sum of
  group rates). Every module rate is a multiple of 10 Hz, so hitting 300000 bit/s needs M ≡ 60 (mod 270),
  and the plan format allows at most 32 modules. The closest loads are 299985 bit/s = 59.997 % (plan 05,
  accept) and 300080 bit/s = 60.016 % (plan 14, reject). Plan 14 goes over only because of its fourth
  heartbeat.
- **PLN-12 cannot be broken on its own.** The 2 Mbit/s link carries up to 226 samples per 10 ms frame
  (22600 Hz total, 1995200 bit/s). A plan within the 60 % CAN limit carries at most about 6.7 kHz of
  module samples plus 2.8 kHz of IMU samples. So the telemetry boundary is tested next to PLN-11:
  plan 16 (22600 Hz, PLN-11 only) and plan 38 (22700 Hz, 2002400 bit/s, PLN-11 and PLN-12).
- **Exactly 2 Mbit/s cannot be reached** with a whole number of samples per frame, so equality is not
  tested.

## Ambiguities found in the documents

Codec / ICDs:

1. The documents do not say whether a decoder enforces the **Range** column (for example `aam_data.mux`
   0..3, the IMU-SER engineering ranges, `time_pulse.leap_s` 0..60, `fix` 0..3, `origin_ns` 0..1e9,
   `diag_request.what` 1..2, `imu_command.cmd` 1..3, `time_quality` 0..3). WRITING_VECTORS.md lists only
   length and count rejects. Every `decode` case keeps its values inside the declared ranges. Type
   extremes appear only in fields that have no range.
2. It is unclear whether range upper bounds are inclusive. `tow_ms` 0..6.048e+08 and `origin_ns` /
   `receive_ns` 0..1e+09 include a value that equals a full week or a full second. Not tested.
3. `time_pulse.leap_s` has type i8 but range 0..60, so the signed type is never used.
4. `time_pulse.fix` has range 0..3, but the value 1 is not described (0 none, 2 2D, 3 3D).
5. The documents do not say whether the PTP decoder checks the header against the message: the
   messageType nibble of `type_sdo`, the version nibble 2, the `length` field against the actual
   payload length, and `domain` 0 (the transport line says domain 0). The vectors always use
   consistent values.
6. IEEE 1588 lets messages carry trailing TLVs or padding, but the ICD gives fixed lengths (44 and 54
   bytes). The set treats only a short PTP payload as a reject and has no "PTP one byte long" case.
7. The documents do not say whether `tlm_packet` is rejected when `magic` is not 0x4B514954. Not tested.
8. The array count fields have no field names ("samples count", "records count"). The set does not
   list the count in `fields`. It is implied by the length of `samples` / `records`. Also, a
   `diag_faults` record has its own field called `count` (the repeats), which is easy to confuse with
   the records count.
9. "trailer crc32" is listed under transport. The set assumes the payload excludes the CRC, as the
   guide says, and that the `22 + 9 × n` length excludes it too.
10. `aam_data` says "+/-32767 is +/- the channel's configured range", but does not say what the raw
    value -32768 means (overrange?). It is decoded as a raw integer here.
11. `aam_reg_request` does not say what `value` holds for a read request (0x40), for example whether
    it must be zero.
12. The CAN ids are built from "node id", and the plan uses "module 1..32". The documents never say
    that the node id is the module number.
13. `diag_status.sw_version` is described as "major<<16 / minor<<8 / patch". The set takes "/" to mean
    bitwise OR.
14. The GNSS transport line is garbled ("framing sync16, pps The rising edge marks ..."). It does not
    affect payloads.

Plan format:

15. Exactly 60 % bus load cannot be reached (see above), so "at most 60 %" is tested at 59.997 % and
    60.016 %.
16. PLN-12 can never be the only rule a plan breaks (see above). The documents also do not say whether
    "fits" includes equality (exactly 2 Mbit/s), or whether Mbit means 10^6 bits. The set assumes
    10^6 bits; equality cannot be reached anyway.
17. Samples per frame is "total sample rate × 10 ms" and is often fractional (for example 42.2). The
    documents do not say whether to use the average or round up to the worst frame, or whether the
    9 bytes per sample are charged on the fractional count. No verdict in this set depends on it.
18. When PLN-08 is broken, the group's rate is not defined, but PLN-11 needs it to compute load.
    Plan 31 keeps the load small either way. Its comment uses the higher rate.
19. The documents do not say whether channels that break other rules still count toward PLN-04
    (channel count), PLN-09, PLN-11 ("modules the plan uses") or PLN-12. For example: a channel with a
    syntax error, module 33, input 12, or an invalid rate. The plans are arranged so that no verdict
    depends on this.
20. The documents do not say whether validation goes on after PLN-01, or whether a missing header
    breaks anything besides PLN-01. The PLN-01 plans (19, 20) are otherwise valid.
21. The number syntax ("decimal, with an optional fraction or exponent") does not mention a sign, but
    PLN-10 needs negative scales, offsets and alarm limits. The set assumes a leading `-` is allowed.
    It does not use `+`, `.5`, `5.` or an upper-case `E`.
22. "An IMU channel's rate is 400, 200, 100 or 50 Hz (whole numbers)": it is unclear whether `400.0`
    or `4e2` is accepted for an IMU channel, given that module rates may be written `5e2`. IMU rates
    in this set are plain integers.
23. `range_mv` is "one of 100, 1000, 5000, 10000" while any number syntax is allowed. Plan 06 assumes
    `1e4` and `10000.0` are accepted.
24. PLN-10 says the alarm limits "lie inside" the measurable range, but not whether the ends count.
    Following the guide's boundary wording, the set treats the ends as inside (plans 10, 11 and 12
    accept limits exactly at the ends).
25. PLN-10 says an IMU channel "measures its ICD range" but does not give the units of IMU alarm
    limits. The set assumes engineering units as in the Range column (rad/s, m/s2, degC), not raw
    counts.
26. "`#` starts a comment": the set assumes this is true anywhere on a line, including right after a
    value with no space (plan 08). Plan 08 also has a trailing comment on the header line. The set
    assumes that is still "exactly `iqk-plan 1`".
27. "Exactly `iqk-plan 1`" might be literal text or two tokens. Tabs and repeated spaces are used only
    on campaign and channel lines (plan 07), not in the header.
28. `unit` must be 1-8 characters, but no rule number is given for breaking this (presumably PLN-02).
    The allowed characters are not given either. Not tested.
29. The documents do not say whether an unknown source (`gnss:week`), a malformed source (`aam:1`,
    `aam:x:0`) or a non-integer module or input (`aam:1.0:0`, leading zeros) is PLN-02 or PLN-05.
    Not tested.
30. Two channels on the same input at different rates would break PLN-09, and arguably PLN-08 too. The
    duplicate-input plans (32, 40) use equal rates to avoid this.
31. `scale=0` together with an alarm would make the measurable range a single point, which would also
    break PLN-10. The PLN-13 plans give no alarm. `scale=-0` is not tested.
32. No character set is given for the campaign id. The set uses `[a-z0-9_]`.
33. The document does not say whether a `key=` with an empty value, an `alarm` with one or three
    values, or a token without `=` is PLN-02. Presumably it is. Not tested.
34. The default for `filter` is not stated (presumably `none`). It does not affect any rule.
