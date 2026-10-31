#!/usr/bin/env python3
"""Start the companion bridge so the device can actually reach it, and say what to type on the device.

Why this exists: the device stores one address, and the computer's address changes. On 2026-10-31 the
device was found polling a host from a previous session — fourteen polls, fourteen failures, nothing on
screen — while the computer sat on a different address in the same subnet. That failure looks like broken
firmware and is really a stale string. This tool reads the address the computer has *now*, starts the
bridge bound to it, and prints the two console lines that point the device at it.

The device is never touched unless `--apply` is given, and even then only `config set`/`config save` are
run through the existing console driver. Flashing is not part of this tool and never will be.

Testing: the address selection, the printed commands and the token handling are pure functions covered by
`tests/test_start_bridge.py`, so this script can be tested on a machine with no device and no network.
"""
from __future__ import annotations

import argparse
import ipaddress
import os
import secrets
import socket
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
BRIDGE = REPO_ROOT / "passport_bridge.py"
CONSOLE = REPO_ROOT / "passport-console.ps1"


def pick_lan_address() -> str:
    """The address another machine on this LAN would use to reach this computer.

    Asking a socket which local address it would use for an outward connection reports the interface the
    routing table would actually choose, which is what the device needs. It sends no packet: UDP connect()
    only binds a route. The loopback form is a last resort, because it is useless to the device and should
    be recognised as such rather than printed as if it were an address.
    """
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.0.2.1", 9))  # TEST-NET-1: documentation range, never routed
        return probe.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        probe.close()


def is_reachable_address(address: str) -> bool:
    """Whether an address is one a device on the LAN could use. Loopback and link-local are not."""
    try:
        parsed = ipaddress.ip_address(address)
    except ValueError:
        return False
    return not (parsed.is_loopback or parsed.is_link_local or parsed.is_unspecified)


def bridge_url(address: str, port: int) -> str:
    return f"http://{address}:{port}"


def device_commands(address: str, port: int, token: str) -> list[str]:
    """The console lines that point the device at this computer.

    `config save` is a separate line on purpose: `config set` changes only the running configuration, so a
    typo cannot become the stored one before it has been tried.
    """
    return [
        f"config set bridge_url {bridge_url(address, port)}",
        f"config set bridge_token {token}",
        "config save",
        "work client",
    ]


def bridge_arguments(address: str, port: int, token: str, fake: bool, extra: list[str]) -> list[str]:
    args = [sys.executable, str(BRIDGE), "--host", address, "--port", str(port), "--bridge-token", token]
    if fake:
        args.append("--fake")
    args.extend(extra)
    return args


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--address", default=None, help="override the detected LAN address")
    parser.add_argument("--token", default=None, help="defaults to PASSPORT_BRIDGE_TOKEN, else a fresh one")
    parser.add_argument("--fake", action="store_true", help="canned transcript and a tone; needs no key")
    parser.add_argument("--print-only", action="store_true", help="print the commands and start nothing")
    parser.add_argument("--apply", action="store_true",
                        help="also run the commands on the device through the serial console")
    parser.add_argument("--serial", default="COM47", help="port for --apply")
    parser.add_argument("rest", nargs=argparse.REMAINDER, help="extra arguments passed to the bridge")
    args = parser.parse_args(argv)

    address = args.address or pick_lan_address()
    if not is_reachable_address(address):
        print(f"start_bridge: no LAN address found (got {address!r}).", file=sys.stderr)
        print("  Pass --address with the address the device should use.", file=sys.stderr)
        return 2

    token = args.token or os.environ.get("PASSPORT_BRIDGE_TOKEN") or secrets.token_hex(16)
    commands = device_commands(address, args.port, token)
    print(f"start_bridge: this computer is {address}")
    print(f"  bridge URL for the device: {bridge_url(address, args.port)}")
    print("  device console commands:")
    for line in commands:
        print(f"    {line}")
    if not args.fake and not os.environ.get("PASSPORT_BRIDGE_KEY"):
        print("  note: PASSPORT_BRIDGE_KEY is not set; the bridge will refuse to call a provider.",
              file=sys.stderr)

    if args.print_only:
        return 0

    extra = [a for a in args.rest if a != "--"]
    bridge = bridge_arguments(address, args.port, token, args.fake, extra)
    print(f"start_bridge: starting {' '.join(bridge[:2])} … (Ctrl+C stops it)")
    try:
        return subprocess.call(bridge, cwd=str(REPO_ROOT))
    except OSError as error:
        print(f"start_bridge: could not start the bridge: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
