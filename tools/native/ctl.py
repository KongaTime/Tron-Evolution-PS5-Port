#!/usr/bin/env python3
"""fefw-ps5 — the console's control payload, in one file.

ps5vkctl (built from PS5_Vulkan/payload/ps5vkctl) listens on 9111, takes one command per line
and answers with one line that starts with "ps5vkctl: ok". Only two commands matter to us:

    kill <TITLEID>    close the running title -- suspend, SIGKILL its processes, sceLncUtilKillApp
    launch <TITLEID>  start it for the foreground user, through sceLncUtilLaunchApp

The success code is not always 0: a launch the console has accepted answers 0x0000e018 (queued)
and the payload confirms it by asking the console which application is running, which is the
answer that counts. So this never judges a launch by its return code -- it prints the line and
lets the caller look at the console's state.

Usage: ctl.py <console-ip> <command> [argument]
"""
import socket
import sys

PORT = 9111


def request(host: str, command: str, timeout: float = 150.0) -> str:
    """Send one command, return the payload's answer line. Raises on a refusal."""
    with socket.create_connection((host, PORT), timeout=30) as connection:
        connection.settimeout(timeout)
        connection.sendall(command.encode("ascii") + b"\n")
        answer = b""
        while b"\n" not in answer:
            chunk = connection.recv(4096)
            if not chunk:
                break
            answer += chunk
    lines = answer.decode("utf-8", "replace").strip().splitlines()
    line = lines[-1] if lines else ""
    # The wire answers "ok ..." or "err ..."; the "ps5vkctl: " a dashboard shows is its own prefix.
    if not (line == "ok" or line.startswith("ok ")):
        raise SystemExit(f"ps5vkctl {command!r}: {line or 'no answer'}")
    return line


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    host, command = sys.argv[1], " ".join(sys.argv[2:])
    print(request(host, command))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
