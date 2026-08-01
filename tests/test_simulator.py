import tempfile
import time
import unittest
from pathlib import Path

from seastars_station.main import build_application


class SimulatorIntegrationTests(unittest.TestCase):
    def test_custom_one_second_auto_start_countdown(self):
        with tempfile.TemporaryDirectory() as directory:
            controller = build_application(
                simulate=True,
                developer_mode=False,
                state_path=Path(directory) / "state.json",
            )
            try:
                controller.save_configuration({
                    "tanks": {
                        "1": {"full_stroke": 1000},
                        "2": {"full_stroke": 1000},
                    },
                    "autonomy": {
                        "auto_start_enabled": True,
                        "auto_start_delay_seconds": 1,
                    },
                })
                controller.connect("SIMULATOR")
                time.sleep(0.25)
                countdown = controller.snapshot()
                self.assertEqual(countdown["mission"]["state"], "COUNTDOWN")
                self.assertFalse(countdown["safety"]["armed"])
                time.sleep(1.15)
                started = controller.snapshot()
                self.assertEqual(started["mission"]["state"], "DIVE")
                self.assertTrue(started["safety"]["armed"])
            finally:
                controller.close()

    def test_simulator_produces_live_telemetry_and_moves(self):
        with tempfile.TemporaryDirectory() as directory:
            controller = build_application(
                simulate=True,
                developer_mode=False,
                state_path=Path(directory) / "state.json",
            )
            try:
                controller.connect("SIMULATOR")
                time.sleep(0.15)
                self.assertTrue(controller.snapshot()["tanks"][0]["fresh"])
                controller.set_full_stroke(1, 1000)
                controller.goto_tank(1, 10)
                time.sleep(0.25)
                self.assertGreater(controller.snapshot()["tanks"][0]["live_position"], 0)
            finally:
                controller.close()

    def test_simulator_starts_open_loop_dive_without_d300_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            controller = build_application(
                simulate=True,
                developer_mode=False,
                state_path=Path(directory) / "state.json",
            )
            try:
                controller.connect("SIMULATOR")
                controller.save_configuration({
                    "tanks": {
                        "1": {"full_stroke": 1000},
                        "2": {"full_stroke": 1000},
                    }
                })
                time.sleep(0.15)
                controller.start_mission()
                time.sleep(0.15)
                status = controller.snapshot()
                self.assertTrue(status["mission"]["active"])
                self.assertEqual(status["mission"]["state"], "DIVE")
                self.assertFalse(status["depth"]["surface_calibrated"])
            finally:
                controller.close()


if __name__ == "__main__":
    unittest.main()
