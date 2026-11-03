# GNSS-SER 1.0.3: GNSS timing receiver, serial time messages and pulse per second

Generated from [`icd/gnss.yaml`](../../icd/gnss.yaml). Do not edit.

Byte order: **little-endian**. Transport: kind serial, baud 115200, framing sync16, pps The rising edge marks the start of a GPS second; the time_pulse message that follows names that second..

## time_pulse

Id 0x20, device to host, 1 Hz, 13 bytes.

Names the second that started at the preceding PPS edge. Arrives within 500 ms of the edge.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | week | u16 |  |  |  | GPS week number. |
| 2 | tow_ms | u32 |  | ms | 0 .. 6.048e+08 | Time of week at the edge. |
| 6 | leap_s | i8 |  | s | 0 .. 60 | GPS minus UTC. |
| 7 | fix | u8 |  |  | 0 .. 3 | 0 none, 2 2D, 3 3D. |
| 8 | flags | u8 |  |  |  | bit 0 time_valid, bit 1 leap_valid, bit 2 raim_ok |
| 9 | acc_ns | u32 |  | ns |  | Estimated time accuracy of the edge. |
