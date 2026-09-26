#!/usr/bin/env python3
"""
Black-box integration tests for the C chat server (server.c / main.c).

Why black-box: almost all server state (clients[], client_count, the mutex)
is `static` inside server.c and not exposed via server.h, so there is nothing
to unit-test in isolation without modifying the production code. Instead,
this test builds the real binary, runs it as a subprocess, and drives it
with real TCP client sockets -- exactly how a real chat client would.

Usage:
    python3 test_chatserver.py

Requires: chatserver binary built alongside this script
    (gcc -Wall -Wextra -pthread -o chatserver main.c server.c)
"""

import os
import socket
import subprocess
import sys
import time

HOST = "127.0.0.1"
BASE_PORT = 8500          # start here and probe upward to dodge collisions
CONNECT_TIMEOUT = 2.0
RECV_TIMEOUT = 2.0
SERVER_BOOT_WAIT = 0.3     # time to let the server bind/listen before connecting

# This script lives in test/, the binary is built to build/server,
# relative to the project root (one level up from this file's directory).
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(PROJECT_ROOT, "build", "server")


class TestFailure(Exception):
    pass


def find_free_port():
    """Ask the OS for a free port so parallel test runs don't collide."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


class ServerProcess:
    """Starts/stops the compiled chatserver binary for one test."""

    def __init__(self, port):
        self.port = port
        self.proc = None

    def __enter__(self):
        if not os.path.isfile(BINARY):
            raise TestFailure(
                f"Binary not found at {BINARY}. Build it first with:\n"
                "  gcc -Wall -Wextra -pthread -o chatserver main.c server.c"
            )
        self.proc = subprocess.Popen(
            [BINARY, str(self.port)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        time.sleep(SERVER_BOOT_WAIT)
        if self.proc.poll() is not None:
            out = self.proc.stdout.read()
            raise TestFailure(f"Server exited immediately (port {self.port} busy?):\n{out}")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


class ChatClient:
    """A minimal test double for a human chat client."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=CONNECT_TIMEOUT)
        self.sock.settimeout(RECV_TIMEOUT)
        self._buf = b""

    def recv_line(self, expect_prefix=None):
        """Read until we get at least one line/prompt. Returns decoded text."""
        try:
            chunk = self.sock.recv(4096)
        except socket.timeout:
            raise TestFailure(f"Timed out waiting for data (expected prefix={expect_prefix!r})")
        if not chunk:
            raise TestFailure("Connection closed unexpectedly while waiting for data")
        text = chunk.decode(errors="replace")
        if expect_prefix and not text.startswith(expect_prefix):
            raise TestFailure(f"Expected message starting with {expect_prefix!r}, got {text!r}")
        return text

    def send_name(self, name):
        self.sock.sendall((name + "\n").encode())

    def send_msg(self, msg):
        self.sock.sendall((msg + "\n").encode())

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# Individual test cases
# --------------------------------------------------------------------------

def test_prompts_for_name(port):
    c = ChatClient(HOST, port)
    try:
        greeting = c.recv_line()
        if "Enter your name" not in greeting:
            raise TestFailure(f"Expected name prompt, got {greeting!r}")
    finally:
        c.close()


def test_join_announcement_broadcast(port):
    """When B joins, A (already connected) should see a join announcement."""
    a = ChatClient(HOST, port)
    b = None
    try:
        a.recv_line()          # "Enter your name: "
        a.send_name("Alice")

        b = ChatClient(HOST, port)
        b.recv_line()          # "Enter your name: "
        b.send_name("Bob")

        # Alice should see Bob's join message (not her own -- she's the "sender" context
        # for join announcements is Bob, so Alice, as a peer, receives it).
        msg = a.recv_line()
        if "Bob joined the chat!" not in msg:
            raise TestFailure(f"Expected Alice to see Bob's join message, got {msg!r}")
    finally:
        a.close()
        if b:
            b.close()


def test_message_broadcast_excludes_sender(port):
    """A message from Alice should reach Bob, but Alice shouldn't get her own echo."""
    a = ChatClient(HOST, port)
    b = None
    try:
        a.recv_line()
        a.send_name("Alice")

        b = ChatClient(HOST, port)
        b.recv_line()
        b.send_name("Bob")
        a.recv_line()  # consume "Bob joined the chat!" seen by Alice

        a.send_msg("hello everyone")

        msg = b.recv_line()
        if not msg.startswith("Alice: hello everyone"):
            raise TestFailure(f"Expected Bob to receive Alice's message, got {msg!r}")

        # Alice should NOT receive her own message back. If she does, recv_line
        # will succeed and we should flag it; if she doesn't, recv times out,
        # which is the correct behavior here.
        a.sock.settimeout(0.5)
        try:
            unexpected = a.sock.recv(4096)
            if unexpected:
                raise TestFailure(
                    f"Sender unexpectedly received their own broadcast message: {unexpected!r}"
                )
        except socket.timeout:
            pass  # expected: no echo to sender
    finally:
        a.close()
        if b:
            b.close()


def test_leave_announcement_broadcast(port):
    """When Bob disconnects, Alice should see a leave announcement."""
    a = ChatClient(HOST, port)
    b = None
    try:
        a.recv_line()
        a.send_name("Alice")

        b = ChatClient(HOST, port)
        b.recv_line()
        b.send_name("Bob")
        a.recv_line()  # "Bob joined the chat!"

        b.close()
        b = None

        msg = a.recv_line()
        if "Bob left the chat." not in msg:
            raise TestFailure(f"Expected Alice to see Bob's leave message, got {msg!r}")
    finally:
        a.close()
        if b:
            b.close()


def test_three_way_broadcast(port):
    """A message from one client should reach BOTH other clients."""
    a = ChatClient(HOST, port)
    b = None
    c = None
    try:
        a.recv_line()
        a.send_name("Alice")

        b = ChatClient(HOST, port)
        b.recv_line()
        b.send_name("Bob")
        a.recv_line()  # Alice sees Bob join

        c = ChatClient(HOST, port)
        c.recv_line()
        c.send_name("Carol")
        a.recv_line()  # Alice sees Carol join
        b.recv_line()  # Bob sees Carol join

        a.send_msg("group message")

        for peer, peer_name in ((b, "Bob"), (c, "Carol")):
            msg = peer.recv_line()
            if not msg.startswith("Alice: group message"):
                raise TestFailure(f"{peer_name} did not receive broadcast, got {msg!r}")
    finally:
        a.close()
        if b:
            b.close()
        if c:
            c.close()


def test_client_disconnect_does_not_crash_server(port, server_proc):
    """Abruptly closing a socket (RST-style) must not take the whole server down.

    This targets the SIGPIPE risk: server writes to a client whose peer already
    vanished. We can't force a RST from Python easily/portably, but we CAN
    verify the server survives a client vanishing mid-session and still serves
    new connections afterward.
    """
    a = ChatClient(HOST, port)
    try:
        a.recv_line()
        a.send_name("Ephemeral")
    finally:
        a.close()  # abrupt close from the test's side

    time.sleep(0.3)
    if server_proc.poll() is not None:
        raise TestFailure("Server process died after a client disconnected")

    # Server should still accept new connections after that.
    b = ChatClient(HOST, port)
    try:
        greeting = b.recv_line()
        if "Enter your name" not in greeting:
            raise TestFailure("Server did not respond normally to a new client after a disconnect")
    finally:
        b.close()


def test_name_with_trailing_crlf_is_trimmed(port):
    """server.c strips \\r\\n from the submitted name (strcspn); verify via join msg."""
    a = ChatClient(HOST, port)
    b = None
    try:
        a.recv_line()
        a.send_name("Alice")

        b = ChatClient(HOST, port)
        b.recv_line()
        b.sock.sendall(b"Bob\r\n")

        msg = a.recv_line()
        if "Bob joined the chat!" not in msg or "Bob\r" in msg:
            raise TestFailure(f"Name was not trimmed correctly, got {msg!r}")
    finally:
        a.close()
        if b:
            b.close()


# --------------------------------------------------------------------------
# Runner
# --------------------------------------------------------------------------

TESTS = [
    test_prompts_for_name,
    test_join_announcement_broadcast,
    test_message_broadcast_excludes_sender,
    test_leave_announcement_broadcast,
    test_three_way_broadcast,
    test_name_with_trailing_crlf_is_trimmed,
]


def run_all():
    passed, failed = 0, 0

    for test_fn in TESTS:
        port = find_free_port()
        name = test_fn.__name__
        try:
            with ServerProcess(port):
                test_fn(port)
            print(f"PASS  {name}")
            passed += 1
        except TestFailure as e:
            print(f"FAIL  {name}: {e}")
            failed += 1
        except Exception as e:  # unexpected error in the test itself
            print(f"ERROR {name}: {type(e).__name__}: {e}")
            failed += 1

    # This one needs the live process handle, so it's run outside the
    # standard loop shape (it also intentionally spans a mid-test disconnect).
    port = find_free_port()
    name = "test_client_disconnect_does_not_crash_server"
    try:
        with ServerProcess(port) as sp:
            test_client_disconnect_does_not_crash_server(port, sp.proc)
        print(f"PASS  {name}")
        passed += 1
    except TestFailure as e:
        print(f"FAIL  {name}: {e}")
        failed += 1
    except Exception as e:
        print(f"ERROR {name}: {type(e).__name__}: {e}")
        failed += 1

    print(f"\n{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(run_all())