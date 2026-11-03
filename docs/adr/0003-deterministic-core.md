# 3. A deterministic core and an imperative shell

## Context
Hardware integration bugs are timing bugs, and they are hard to reproduce. A unit that reads the clock and
the sockets wherever it likes cannot be replayed.

## Decision
`iqk::Dau` takes every input with the local time it happened and returns bytes to send. It never reads a
clock, starts a thread or allocates on the acquisition path. `daud` owns the clock, serial ports and sockets,
and records every input and tick. `iqkctl replay` feeds a recording through a new core and must produce the
same telemetry, byte for byte (SRS-100).

## Consequences
Any field problem that was recorded can be replayed on a desk and in CI. Unit tests drive the core at exact
times (1 ms steps, held-up frames, clock drift) without sleeping. Software time stamps are the price: on this
bench they are taken in user space; on the target the PPS edge and PTP packets would be stamped by the kernel
or the NIC, and the thresholds in `config/unit.yaml` say so.
