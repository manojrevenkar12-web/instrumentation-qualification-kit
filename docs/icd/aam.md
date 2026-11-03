# AAM-CAN 2.0.1: Analog acquisition module, classic CAN 500 kbit/s

Generated from [`icd/aam.yaml`](../../icd/aam.yaml). Do not edit.

Byte order: **little-endian**. Transport: kind can, bitrate 500000, id_bits 11.

- `channels`: 12
- `channel_groups`: 4
- `base_rate_hz`: 1000
- `divisors`: [1, 2, 5, 10, 20, 50, 100]
- `ranges_mv`: [100, 1000, 5000, 10000]
- `filters`: ['none', 'lp_100hz', 'lp_10hz']

## Registers

Per-channel registers are at index + channel (0..channels-1).

| Index | Sub | Name | Values | Notes |
|---|---:|---|---|---|
| 0x2000 + ch | 1 | ch_enable | 0..1 |  |
| 0x2000 + ch | 2 | ch_range_code | 0..3 |  |
| 0x2000 + ch | 3 | ch_divisor | one of `divisors` |  |
| 0x2000 + ch | 4 | ch_filter | 0..2 |  |
| 0x1F00 | 0 | commit |  | Writing 1 applies the configuration and computes its CRC. |
| 0x1F01 | 0 | config_crc |  | CRC-32 (IEEE) of the applied configuration, read only: for channels 0..11 in order, enable, range code, divisor and filter, each as u32 little-endian. |

## aam_heartbeat

Id 0x700 + node id, device to host, 1 Hz, 4 bytes.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | state | u8 |  |  | 0 .. 3 | 0 boot, 1 pre-operational, 2 operational, 3 fault. |
| 1 | counter | u8 |  |  |  |  |
| 2 | errors | u16 |  |  |  | bit 0 adc_fault, bit 1 overrange, bit 2 cold_junction, bit 3 config_crc, bit 4 bus_warning |

## aam_data

Id 0x180 + node id, device to host, 8 bytes.

Three channels of group mux. Raw counts; +/-32767 is +/- the channel's configured range.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | mux | u8 |  |  | 0 .. 3 |  |
| 1 | counter | u8 |  |  |  | Increments per frame per group |
| 2 | ch_a | i16 |  |  |  |  |
| 4 | ch_b | i16 |  |  |  |  |
| 6 | ch_c | i16 |  |  |  |  |

## aam_reg_request

Id 0x600 + node id, host to device, 8 bytes.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | cmd | u8 |  |  |  | 0x23 write, 0x40 read. |
| 1 | index | u16 |  |  |  |  |
| 3 | sub | u8 |  |  |  |  |
| 4 | value | u32 |  |  |  |  |

## aam_reg_response

Id 0x580 + node id, device to host, 8 bytes.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | cmd | u8 |  |  |  | 0x60 write acknowledged, 0x43 read value, 0x80 abort (value is the abort code). |
| 1 | index | u16 |  |  |  |  |
| 3 | sub | u8 |  |  |  |  |
| 4 | value | u32 |  |  |  |  |

## aam_nmt

Id 0x0, host to device, 2 bytes.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | cmd | u8 |  |  |  | 0x01 start, 0x02 stop, 0x80 pre-operational, 0x81 reset. |
| 1 | node | u8 |  |  |  |  |
