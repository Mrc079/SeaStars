import unittest

from seastars_station.protocol import (
    DepthEvent,
    ImuEvent,
    ImuStatusEvent,
    MessageEvent,
    MissionEvent,
    PositionEvent,
    ProtocolError,
    configure,
    move_tank,
    parse_line,
    set_thruster,
)


class ProtocolTests(unittest.TestCase):
    def test_parses_position_with_spacing(self):
        self.assertEqual(parse_line("POS1: -12   POS2: 345"), PositionEvent(-12, 345))

    def test_parses_decimal_imu(self):
        self.assertEqual(parse_line("IMU H:12.5 R:-3.2 P:4 C:255"), ImuEvent(12.5, -3.2, 4.0, 255))

    def test_parses_i2c_imu_identity(self):
        self.assertEqual(
            parse_line("SENSOR IMU:BNO055_I2C ADDR:0x28"),
            ImuStatusEvent("BNO055_I2C", 0x28, True),
        )
        self.assertEqual(parse_line("SENSOR IMU:LOST"), ImuStatusEvent(None, None, False))

    def test_unknown_line_is_preserved(self):
        self.assertEqual(parse_line("ARMED"), MessageEvent("ARMED"))

    def test_parses_d300_and_mission_telemetry(self):
        self.assertEqual(
            parse_line("DEPTH M:0.301 T:22.4 P:1042.7 S:0.181 Z:1"),
            DepthEvent(0.301, 22.4, 1042.7, 0.181, True),
        )
        self.assertEqual(
            parse_line("AUTO STATE:STRAIGHT_2 ELAPSED:15100 FAULT:NONE ACTIVE:1"),
            MissionEvent("STRAIGHT_2", 15100, "NONE", True),
        )

    def test_command_builders_reject_unsafe_values(self):
        with self.assertRaises(ProtocolError):
            move_tank(3, 100)
        with self.assertRaises(ProtocolError):
            set_thruster(1, 101)

    def test_thruster_uses_legacy_scale(self):
        self.assertEqual(set_thruster(2, -50), "T2 -100")

    def test_open_loop_autonomy_configuration_commands(self):
        self.assertEqual(configure("ROUTE", 1), "CFG ROUTE 1")
        self.assertEqual(configure("DIVE_BALLAST_PCT", 34), "CFG DIVE_BALLAST_PCT 34")
        self.assertEqual(configure("AUTO_DELAY_MS", 7000), "CFG AUTO_DELAY_MS 7000")


if __name__ == "__main__":
    unittest.main()
