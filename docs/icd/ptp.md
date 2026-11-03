# PTP-UDP 1.1.0: IEEE 1588 precision time protocol, two-step, end-to-end

Generated from [`icd/ptp.yaml`](../../icd/ptp.yaml). Do not edit.

Byte order: **big-endian**. Transport: kind udp, event_port 319, general_port 320, domain 0.

## ptp_sync

Id 0x0, master to slave, 8 Hz, 44 bytes.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | type_sdo | u8 |  |  |  | majorSdoId (high nibble) and messageType (low nibble): 0 Sync, 1 Delay_Req, 8 Follow_Up, 9 Delay_Resp. |
| 1 | version | u8 |  |  |  | versionPTP 2 (low nibble). |
| 2 | length | u16 |  |  |  |  |
| 4 | domain | u8 |  |  |  |  |
| 5 | minor_sdo | u8 |  |  |  |  |
| 6 | flags | u16 |  |  |  | bit 9 two_step |
| 8 | correction | i64 |  |  |  | Scaled nanoseconds (ns * 2^16). |
| 16 | type_specific | u32 |  |  |  |  |
| 20 | clock_identity | u64 |  |  |  |  |
| 28 | port_number | u16 |  |  |  |  |
| 30 | sequence_id | u16 |  |  |  |  |
| 32 | control | u8 |  |  |  |  |
| 33 | log_interval | i8 |  |  |  |  |
| 34 | origin_s | u48 |  |  |  |  |
| 40 | origin_ns | u32 |  |  | 0 .. 1e+09 |  |

## ptp_delay_req

Id 0x1, slave to master, 44 bytes.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | type_sdo | u8 |  |  |  | majorSdoId (high nibble) and messageType (low nibble): 0 Sync, 1 Delay_Req, 8 Follow_Up, 9 Delay_Resp. |
| 1 | version | u8 |  |  |  | versionPTP 2 (low nibble). |
| 2 | length | u16 |  |  |  |  |
| 4 | domain | u8 |  |  |  |  |
| 5 | minor_sdo | u8 |  |  |  |  |
| 6 | flags | u16 |  |  |  | bit 9 two_step |
| 8 | correction | i64 |  |  |  | Scaled nanoseconds (ns * 2^16). |
| 16 | type_specific | u32 |  |  |  |  |
| 20 | clock_identity | u64 |  |  |  |  |
| 28 | port_number | u16 |  |  |  |  |
| 30 | sequence_id | u16 |  |  |  |  |
| 32 | control | u8 |  |  |  |  |
| 33 | log_interval | i8 |  |  |  |  |
| 34 | origin_s | u48 |  |  |  |  |
| 40 | origin_ns | u32 |  |  | 0 .. 1e+09 |  |

## ptp_follow_up

Id 0x8, master to slave, 44 bytes.

Carries t1, the precise transmit time of the Sync with the same sequence_id.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | type_sdo | u8 |  |  |  | majorSdoId (high nibble) and messageType (low nibble): 0 Sync, 1 Delay_Req, 8 Follow_Up, 9 Delay_Resp. |
| 1 | version | u8 |  |  |  | versionPTP 2 (low nibble). |
| 2 | length | u16 |  |  |  |  |
| 4 | domain | u8 |  |  |  |  |
| 5 | minor_sdo | u8 |  |  |  |  |
| 6 | flags | u16 |  |  |  | bit 9 two_step |
| 8 | correction | i64 |  |  |  | Scaled nanoseconds (ns * 2^16). |
| 16 | type_specific | u32 |  |  |  |  |
| 20 | clock_identity | u64 |  |  |  |  |
| 28 | port_number | u16 |  |  |  |  |
| 30 | sequence_id | u16 |  |  |  |  |
| 32 | control | u8 |  |  |  |  |
| 33 | log_interval | i8 |  |  |  |  |
| 34 | origin_s | u48 |  |  |  |  |
| 40 | origin_ns | u32 |  |  | 0 .. 1e+09 |  |

## ptp_delay_resp

Id 0x9, master to slave, 54 bytes.

Carries t4, when the master received the Delay_Req with the same sequence_id.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | type_sdo | u8 |  |  |  | majorSdoId (high nibble) and messageType (low nibble): 0 Sync, 1 Delay_Req, 8 Follow_Up, 9 Delay_Resp. |
| 1 | version | u8 |  |  |  | versionPTP 2 (low nibble). |
| 2 | length | u16 |  |  |  |  |
| 4 | domain | u8 |  |  |  |  |
| 5 | minor_sdo | u8 |  |  |  |  |
| 6 | flags | u16 |  |  |  | bit 9 two_step |
| 8 | correction | i64 |  |  |  | Scaled nanoseconds (ns * 2^16). |
| 16 | type_specific | u32 |  |  |  |  |
| 20 | clock_identity | u64 |  |  |  |  |
| 28 | port_number | u16 |  |  |  |  |
| 30 | sequence_id | u16 |  |  |  |  |
| 32 | control | u8 |  |  |  |  |
| 33 | log_interval | i8 |  |  |  |  |
| 34 | receive_s | u48 |  |  |  |  |
| 40 | receive_ns | u32 |  |  | 0 .. 1e+09 |  |
| 44 | req_clock_identity | u64 |  |  |  |  |
| 52 | req_port_number | u16 |  |  |  |  |
