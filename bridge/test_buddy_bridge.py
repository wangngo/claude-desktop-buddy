"""Tests for rate-limit selection. Run: bridge/.venv/bin/python -m unittest discover -s bridge -v"""
import unittest

from buddy_bridge import pick_rate

WEEK_A = 1795608000   # acc A weekly reset
WEEK_B = 1795700000   # acc B weekly reset
H5 = 1795254600


def rl(h5_pct, h5_reset, d7_pct, d7_reset):
    return {"five_hour": {"used_percentage": h5_pct, "resets_at": h5_reset},
            "seven_day": {"used_percentage": d7_pct, "resets_at": d7_reset}}


class PickRate(unittest.TestCase):
    def test_same_account_takes_highest_not_last_active(self):
        # Real case 2026-11-21: stale session at 1%, live one at 3%, same account.
        by = {"old": (rl(1, H5, 61, WEEK_A), 100), "new": (rl(3, H5, 61, WEEK_A), 100)}
        active = {"old": 200, "new": 150}   # stale session fired a hook last
        out = pick_rate(by, active)
        self.assertEqual(out["five_hour"]["used_percentage"], 3)
        self.assertEqual(out["seven_day"]["used_percentage"], 61)

    def test_two_accounts_follow_most_recent_activity(self):
        by = {"a": (rl(50, H5, 61, WEEK_A), 100), "b": (rl(10, H5 + 999, 20, WEEK_B), 100)}
        out = pick_rate(by, {"a": 100, "b": 300})
        self.assertEqual(out["five_hour"]["used_percentage"], 10)
        self.assertEqual(out["seven_day"]["resets_at"], WEEK_B)
        out = pick_rate(by, {"a": 400, "b": 300})
        self.assertEqual(out["five_hour"]["used_percentage"], 50)

    def test_new_5h_window_beats_old_window_snapshot(self):
        by = {"old": (rl(80, H5, 61, WEEK_A), 100), "new": (rl(0, H5 + 18000, 62, WEEK_A), 100)}
        out = pick_rate(by, {"old": 200, "new": 150})
        self.assertEqual(out["five_hour"], {"used_percentage": 0, "resets_at": H5 + 18000})
        self.assertEqual(out["seven_day"]["used_percentage"], 62)


if __name__ == "__main__":
    unittest.main()
