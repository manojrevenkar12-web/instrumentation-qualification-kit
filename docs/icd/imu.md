# IMU-SER 1.2.0: Inertial measurement unit, RS-422 serial

Generated from [`icd/imu.yaml`](../../icd/imu.yaml). Do not edit.

Byte order: **little-endian**. Transport: kind serial, baud 921600, framing sync16.

## imu_sample

Id 0x1, device to host, 400 Hz, 33 bytes.

One inertial sample, taken at the device's sample instant t_us.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | seq | u16 |  |  |  | Sample counter |
| 2 | t_us | u32 |  | us |  | Device time of the sample |
| 6 | gyro_x | i32 | 1e-06 | rad/s | -8.727 .. 8.727 |  |
| 10 | gyro_y | i32 | 1e-06 | rad/s | -8.727 .. 8.727 |  |
| 14 | gyro_z | i32 | 1e-06 | rad/s | -8.727 .. 8.727 |  |
| 18 | accel_x | i32 | 1e-05 | m/s2 | -156.9 .. 156.9 |  |
| 22 | accel_y | i32 | 1e-05 | m/s2 | -156.9 .. 156.9 |  |
| 26 | accel_z | i32 | 1e-05 | m/s2 | -156.9 .. 156.9 |  |
| 30 | temp | i16 | 0.01 | degC | -40 .. 85 |  |
| 32 | status | u8 |  |  |  | bit 0 gyro_ok, bit 1 accel_ok, bit 2 temp_ok, bit 3 saturated |

## imu_bit_result

Id 0x2, device to host, 3 bytes.

Result of a commanded self-test. result 0 = pass.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | result | u8 |  |  |  |  |
| 1 | code | u16 |  |  |  | Failing sub-test |

## imu_command

Id 0x10, host to device, 3 bytes.

Commands: 1 start streaming, 2 stop streaming, 3 run self-test.

| Offset | Field | Type | Scale | Unit | Range | Notes |
|---:|---|---|---|---|---|---|
| 0 | cmd | u8 |  |  | 1 .. 3 |  |
| 1 | arg | u16 |  |  |  |  |
