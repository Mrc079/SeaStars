import tempfile
import time
import unittest
from pathlib import Path

from seastars_station.controller import StationController, StationError
from seastars_station.state_store import StateStore


class FakeTransport:
    def __init__(self):
        self.connected = True
        self.port = "TEST"
        self.commands = []
        self.fail_next = False

    def list_ports(self):
        return ["TEST"]

    def connect(self, port):
        self.connected = True
        self.port = port

    def disconnect(self):
        self.connected = False
        self.port = None

    def send(self, command):
        if self.fail_next:
            self.fail_next = False
            return False
        self.commands.append(command)
        return True


class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.transport = FakeTransport()
        self.controller = StationController(
            self.transport,
            StateStore(Path(self.temp.name) / "state.json"),
            heartbeat_timeout=0.15,
        )
        self.controller.on_transport_state(True, None)
        self.controller.on_line("POS1:0 POS2:0 Z1:1 Z2:1")

    def tearDown(self):
        self.controller.close()
        self.temp.cleanup()

    def test_uncalibrated_tank_cannot_use_percent(self):
        with self.assertRaisesRegex(StationError, "kalibre"):
            self.controller.goto_tank(1, 50)

    def test_failed_send_does_not_change_target(self):
        self.controller.set_full_stroke(1, 1000)
        self.transport.fail_next = True
        with self.assertRaises(StationError):
            self.controller.goto_tank(1, 50)
        self.assertEqual(self.controller.snapshot()["tanks"][0]["target_position"], 0)

    def test_goto_uses_live_position_and_legacy_command(self):
        self.controller.set_full_stroke(1, 1000)
        self.controller.on_line("POS1:200 POS2:0")
        self.controller.goto_tank(1, 50)
        self.assertEqual(self.transport.commands[-1], "S1 300")

    def test_percentage_target_requires_physical_zero(self):
        self.controller.set_full_stroke(1, 1000)
        self.controller.on_line("POS1:0 POS2:0 Z1:0 Z2:1")
        with self.assertRaisesRegex(StationError, "sıfırlanmalı"):
            self.controller.goto_tank(1, 35)

    def test_stale_tank_telemetry_blocks_motion(self):
        self.controller.telemetry_timeout = 0.01
        self.controller.set_full_stroke(1, 1000)
        time.sleep(0.02)
        with self.assertRaisesRegex(StationError, "telemetrisi"):
            self.controller.goto_tank(1, 50)

    def test_first_manual_propulsion_command_auto_arms(self):
        self.controller.set_thruster(1, 20)
        self.assertEqual(self.transport.commands[-2:], ["ARM", "T1 40"])
        self.assertTrue(self.controller.snapshot()["safety"]["armed"])

    def test_heartbeat_timeout_disarms_and_zeros(self):
        self.controller.arm()
        self.controller.set_thruster(1, 20)
        time.sleep(0.3)
        self.assertFalse(self.controller.snapshot()["safety"]["armed"])
        self.assertIn("DISARM", self.transport.commands)

    def test_estop_is_latched_until_explicit_clear(self):
        self.controller.emergency_stop()
        with self.assertRaisesRegex(StationError, "kilidi"):
            self.controller.arm()
        self.controller.clear_estop()
        self.assertFalse(self.controller.snapshot()["safety"]["estop_latched"])

    def test_thruster_pair_is_sent_as_one_validated_operation(self):
        self.controller.arm()
        self.controller.set_thruster_pair(-20, 25)
        self.assertEqual(self.transport.commands[-2:], ["T1 -40", "T2 50"])
        self.assertEqual(self.controller.snapshot()["thrusters"][1]["percent"], 25)

    def test_heading_zero_is_persisted_and_sent_to_stm32(self):
        self.controller.on_line("IMU H:350 R:0 P:0 C:255")
        offset = self.controller.calibrate_heading_zero()
        self.assertAlmostEqual(offset, 10.0)
        self.assertEqual(self.transport.commands[-1], "CFG HEADING_OFFSET_CDEG 1000")
        self.assertAlmostEqual(self.controller.configuration()["imu"]["heading_offset_deg"], 10.0)

    def test_i2c_imu_identity_is_exposed_to_browser(self):
        self.controller.on_line("SENSOR IMU:BNO085_I2C ADDR:0x4A")
        imu = self.controller.snapshot()["imu"]
        self.assertEqual(imu["model"], "BNO085_I2C")
        self.assertEqual(imu["address"], 0x4A)
        self.assertTrue(imu["detected"])

    def test_depth_zero_requires_live_d300(self):
        with self.assertRaisesRegex(StationError, "D300"):
            self.controller.calibrate_depth_surface()

    def test_mission_no_longer_depends_on_d300_surface_zero(self):
        self.controller.set_full_stroke(1, 1000)
        self.controller.set_full_stroke(2, 1000)
        self.controller.on_line("IMU H:0 R:0 P:0 C:255")
        self.controller.on_line("DEPTH M:0 T:20 P:1013.2 S:0 Z:0")
        self.controller.start_mission()
        self.assertEqual(self.transport.commands[-1], "AUTO START")

    def test_mission_starts_after_all_sensor_interlocks(self):
        self.controller.set_full_stroke(1, 1000)
        self.controller.set_full_stroke(2, 1000)
        self.controller.on_line("IMU H:0 R:0 P:0 C:255")
        self.controller.on_line("DEPTH M:0 T:20 P:1013.2 S:0 Z:1")
        self.controller.start_mission()
        self.assertEqual(self.transport.commands[-1], "AUTO START")
        self.assertTrue(self.controller.snapshot()["mission"]["active"])

    def test_web_configuration_rejects_unknown_sections(self):
        with self.assertRaisesRegex(StationError, "Bilinmeyen"):
            self.controller.save_configuration({"mystery": {"value": 1}})

    def test_enabled_custom_second_delay_prepares_countdown_without_arming(self):
        self.controller.save_configuration({
            "tanks": {
                "1": {"full_stroke": 1000},
                "2": {"full_stroke": 1000},
            },
            "autonomy": {
                "auto_start_enabled": True,
                "auto_start_delay_seconds": 7,
            },
        })
        self.assertIn("CFG AUTO_DELAY_MS 7000", self.transport.commands)
        self.assertEqual(self.transport.commands[-1], "AUTO PREPARE")
        status = self.controller.snapshot()
        self.assertEqual(status["mission"]["state"], "COUNTDOWN")
        self.assertTrue(status["mission"]["active"])
        self.assertFalse(status["safety"]["armed"])

    def test_disabled_auto_start_keeps_custom_delay_without_scheduling(self):
        self.controller.save_configuration({
            "autonomy": {
                "auto_start_enabled": False,
                "auto_start_delay_seconds": 13,
            }
        })
        self.assertIn("CFG AUTO_DELAY_MS 0", self.transport.commands)
        self.assertNotIn("AUTO PREPARE", self.transport.commands)
        self.assertEqual(
            self.controller.configuration()["autonomy"]["auto_start_delay_seconds"], 13
        )


if __name__ == "__main__":
    unittest.main()
