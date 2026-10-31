"""Tests for the bridge launcher's decision logic, without a device, a bridge or a provider.

The launcher exists because a stale address costs an evening of diagnosis while looking exactly like
broken firmware. What it prints is therefore the part worth testing: the address it would tell the device
to use, the commands, and the refusal when the only address available is one the device cannot use.
"""
import importlib.util
import os
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("start_bridge", REPO / "tools" / "start_bridge.py")
launcher = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(launcher)


class AddressRules(unittest.TestCase):
    def test_loopback_is_not_offered_to_a_device(self):
        self.assertFalse(launcher.is_reachable_address("127.0.0.1"))
        self.assertFalse(launcher.is_reachable_address("127.1.2.3"))

    def test_link_local_and_unspecified_are_refused(self):
        self.assertFalse(launcher.is_reachable_address("169.254.10.4"))
        self.assertFalse(launcher.is_reachable_address("0.0.0.0"))

    def test_lan_addresses_are_accepted(self):
        for address in ("192.168.2.137", "192.168.3.75", "10.0.0.5", "172.20.208.1"):
            self.assertTrue(launcher.is_reachable_address(address), address)

    def test_nonsense_is_refused_rather_than_printed(self):
        for address in ("", "not-an-address", "192.168.2.300", "::1"):
            self.assertFalse(launcher.is_reachable_address(address))

    def test_detection_returns_something_parsable(self):
        # Whatever this machine has, the result must be an address, not an exception or an empty string.
        address = launcher.pick_lan_address()
        self.assertIsInstance(address, str)
        self.assertTrue(address)
        launcher.is_reachable_address(address)  # must not raise for the fallback either


class PrintedCommands(unittest.TestCase):
    def test_url_shape(self):
        self.assertEqual(launcher.bridge_url("192.168.2.137", 8080), "http://192.168.2.137:8080")

    def test_commands_set_both_fields_and_save(self):
        commands = launcher.device_commands("192.168.2.137", 8080, "abc123")
        self.assertIn("config set bridge_url http://192.168.2.137:8080", commands)
        self.assertIn("config set bridge_token abc123", commands)
        # Saving is its own step so a typo cannot become the stored configuration.
        self.assertIn("config save", commands)
        self.assertLess(commands.index("config set bridge_token abc123"), commands.index("config save"))

    def test_commands_are_ascii(self):
        # The console mangles non-ASCII input; anything printed for it must be plain.
        for line in launcher.device_commands("10.0.0.5", 8080, "t0ken"):
            line.encode("ascii")


class BridgeArguments(unittest.TestCase):
    def test_binds_to_the_announced_address(self):
        args = launcher.bridge_arguments("192.168.2.137", 8080, "tok", False, [])
        self.assertIn("--host", args)
        self.assertEqual(args[args.index("--host") + 1], "192.168.2.137")
        self.assertEqual(args[args.index("--port") + 1], "8080")
        self.assertEqual(args[args.index("--bridge-token") + 1], "tok")
        self.assertNotIn("--fake", args)

    def test_fake_is_opt_in(self):
        args = launcher.bridge_arguments("10.0.0.5", 9000, "tok", True, ["--stt-delay", "3"])
        self.assertIn("--fake", args)
        self.assertIn("--stt-delay", args)


class CommandLine(unittest.TestCase):
    def run_cli(self, *args, env=None):
        return subprocess.run(
            [sys.executable, str(REPO / "tools" / "start_bridge.py"), *args],
            capture_output=True, text=True, timeout=60, env=env,
        )

    def test_print_only_starts_nothing(self):
        result = self.run_cli("--print-only", "--address", "192.168.2.137", "--token", "deadbeef")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("http://192.168.2.137:8080", result.stdout)
        self.assertIn("config set bridge_token deadbeef", result.stdout)
        # Print-only must not claim to have started anything.
        self.assertNotIn("starting", result.stdout)

    def test_unusable_address_is_refused_with_help(self):
        result = self.run_cli("--print-only", "--address", "127.0.0.1", "--token", "x")
        self.assertEqual(result.returncode, 2)
        self.assertIn("no LAN address", result.stderr)

    def test_missing_provider_key_is_stated_not_hidden(self):
        env = {k: v for k, v in os.environ.items() if k != "PASSPORT_BRIDGE_KEY"}
        result = self.run_cli("--print-only", "--address", "192.168.2.137", "--token", "x", env=env)
        self.assertEqual(result.returncode, 0)
        self.assertIn("PASSPORT_BRIDGE_KEY is not set", result.stderr)


if __name__ == "__main__":
    unittest.main()
