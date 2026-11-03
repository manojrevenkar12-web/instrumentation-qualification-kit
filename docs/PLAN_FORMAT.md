# Measurement plan format

A measurement plan tells the data acquisition unit (DAU) what to acquire. Planning tools write it; the unit
parses and validates it, and refuses it if any rule below is broken. Nothing is configured from a refused plan.

## Syntax

One statement per line. `#` starts a comment. Blank lines are ignored. Tokens are separated by spaces or tabs.

```
iqk-plan 1
campaign <id>
channel <name> aam:<module>:<input> rate=<hz> range_mv=<mv> [filter=<f>] [scale=<k>] [offset=<c>] [unit=<u>] [alarm=<lo>,<hi>]
channel <name> imu:<field> rate=<hz> [alarm=<lo>,<hi>]
```

- The first statement must be exactly `iqk-plan 1` (**PLN-01**).
- Exactly one `campaign` statement with one token. Any other statement, an unknown key for the source, a key
  given twice, a value that is not a number where a number is expected (`10Hz`, `nan`, `inf`), a missing
  `rate`, or a missing `range_mv` on a module channel is a syntax error (**PLN-02**).
- Numbers are decimal, with an optional fraction or exponent (`500`, `500.0`, `5e2`).
- `scale` is engineering units per volt (default 1), `offset` is added after scaling (default 0), `unit` is
  1-8 characters (default `V`). These three keys and `filter`, `range_mv` exist only for module channels.

## Rules

| Rule | What must hold |
|---|---|
| PLN-03 | Channel names are 1-16 characters of `a-z`, `0-9` and `_`, and unique in the plan. |
| PLN-04 | A plan has 1 to 48 channels. |
| PLN-05 | Module channels name a module 1..32 and an input 0..11. IMU channels name one of `gyro_x`, `gyro_y`, `gyro_z`, `accel_x`, `accel_y`, `accel_z`, `temp`. |
| PLN-06 | A module channel's rate is 1000 Hz divided by one of 1, 2, 5, 10, 20, 50, 100. An IMU channel's rate is 400, 200, 100 or 50 Hz (whole numbers). |
| PLN-07 | `range_mv` is one of 100, 1000, 5000, 10000 (full scale is plus or minus that). `filter` is `none`, `lp_100hz` or `lp_10hz`. |
| PLN-08 | A module sends inputs 0-2, 3-5, 6-8 and 9-11 in one data frame each, so channels on inputs of the same group of the same module must have the same rate. |
| PLN-09 | No module input and no IMU field is used by two channels. |
| PLN-10 | With `alarm=lo,hi`: lo < hi, and both lie inside what the channel can measure. A module channel measures scale × (±range_mv/1000 V) + offset (the lower of the two ends is the minimum when scale is negative). An IMU channel measures its ICD range (IMU-SER). |
| PLN-11 | The worst-case load the plan puts on the CAN bus is at most 60 %. Every module the plan uses sends a 4-byte heartbeat at 1 Hz; every group with at least one planned input sends an 8-byte data frame at the group's rate. A classic CAN frame with an 11-bit identifier and n data bytes takes, worst case with bit stuffing, 8n + 47 + floor((34 + 8n - 1) / 4) bits. The bus runs at 500 kbit/s and all modules share it. |
| PLN-12 | The telemetry the plan produces fits the 2 Mbit/s telemetry link. Output frames are sent every 10 ms; each sample costs 9 bytes; each packet carries up to 48 samples and costs 22 + 4 bytes of header and CRC plus 66 bytes of UDP, IP and Ethernet overhead; samples per frame is the plan's total sample rate (all channels) times 10 ms, and a frame needs ceil(samples per frame / 48) packets, at least one. |
| PLN-13 | `scale` is not zero. |

The unit reports every finding with its rule and line; a plan can break several rules at once.
