# 5. Servo gains and lock thresholds per time source

## Context
One PI servo tuned for PTP at 8 Hz never locked to GNSS on the bench: one measurement a second with a PPS edge
that carries about 200 us of software jitter, amplified by gains meant for a fast, clean source, swung the
clock rate by up to 200 ppm.

## Decision
`config/unit.yaml` holds gains and lock/unlock thresholds per source (PTP: kp 0.7, ki 0.3, lock 100 us;
GNSS: kp 0.3, ki 0.05, lock 400 us). Offsets above 1 ms are stepped; below, the clock is slewed so time never
runs backwards.

## Consequences
GNSS-only operation locks and holds stamps within 1 ms at the 99th percentile on the bench. With hardware time
stamping both thresholds would be about 1 us; they are settings, not code.
