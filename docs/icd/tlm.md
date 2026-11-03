# DAU-UDP 1.3.0: Data acquisition unit telemetry and diagnostics, UDP/IPv4

Generated from [`icd/tlm.yaml`](../../icd/tlm.yaml). Do not edit.

Byte order: **little-endian**. Transport: kind udp, telemetry_port 47000, diag_port 47001, trailer crc32.

## tlm_packet

Id 0x50, device to host, 22 + 9 × n bytes (n ≤ 48).

One packet per output frame. Samples carry the time of acquisition, not of sending.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | magic | u32 |  |  |  | 0x4B514954 ('TIQK'). |
| 4 | seq | u32 |  |  |  |  |
| 8 | time_ns | u64 |  | ns |  | UTC nanoseconds of the frame's first sample. |
| 16 | time_quality | u8 |  |  | 0 .. 3 | 0 free-running, 1 holdover, 2 locked to GNSS, 3 locked to PTP. |
| 17 | plan_crc | u32 |  |  |  | CRC-32 of the measurement plan in force. |
| 21 | samples count | u8 | | | 0..48 | |
| +0 | samples[i].channel | u16 |  |  | |  |
| +2 | samples[i].dt_us | u16 |  | us | | Offset from time_ns. |
| +4 | samples[i].value | i32 | 0.0001 |  | | Engineering units per the plan. |
| +8 | samples[i].flags | u8 |  |  | | bit 0 stale, bit 1 overrange, bit 2 alarm_low, bit 3 alarm_high |

## diag_request

Id 0x40, host to device, 1 bytes.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | what | u8 |  |  | 1 .. 2 | 1 status, 2 fault log. |

## diag_status

Id 0x41, device to host, 35 bytes.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | uptime_s | u32 |  |  |  |  |
| 4 | sw_version | u32 |  |  |  | major<<16 / minor<<8 / patch. |
| 8 | slot | u8 |  |  |  | Boot slot |
| 9 | bit | u16 |  |  |  | bit 0 imu, bit 1 aam, bit 2 gnss, bit 3 ptp, bit 4 plan, bit 5 storage |
| 11 | time_state | u8 |  |  |  | 0 free-running, 1 locking, 2 locked, 3 holdover. |
| 12 | time_source | u8 |  |  |  | 0 none, 1 GNSS, 2 PTP. |
| 13 | offset_ns | i32 |  |  |  |  |
| 17 | imu_crc_errors | u32 |  |  |  |  |
| 21 | imu_gaps | u32 |  |  |  |  |
| 25 | can_errors | u32 |  |  |  |  |
| 29 | plan_crc | u32 |  |  |  |  |
| 33 | faults_logged | u16 |  |  |  |  |

## diag_faults

Id 0x42, device to host, 5 + 12 × n bytes (n ≤ 32).

The most recent fault records, oldest first.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | total | u32 |  |  |  | Faults logged since boot |
| 4 | records count | u8 | | | 0..32 | |
| +0 | records[i].local_ms | u32 |  | ms | | Unit uptime when the fault was first seen. |
| +4 | records[i].code | u16 |  |  | |  |
| +6 | records[i].detail | i32 |  |  | |  |
| +10 | records[i].count | u16 |  |  | | Repeats folded into this record. |
