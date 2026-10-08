#!/usr/bin/env python3
"""Raw math-speak peer for real_server.sh: misbehaves on purpose, or finishes a session slowly.

Prints every byte the server sent after the misbehaviour (expected: none), or the BYE line.
"""
import argparse
import socket
import sys
import time

PROTOCOL = "cs230"
RECEIVE_CHUNK = 4096


def read_line(connection, buffer):
    """Returns the next line (without newline) and the leftover bytes, or None at EOF."""
    while b"\n" not in buffer:
        chunk = connection.recv(RECEIVE_CHUNK)
        if not chunk:
            return None, buffer
        buffer += chunk
    line, _, buffer = buffer.partition(b"\n")
    return line.decode(), buffer


def drain(connection):
    """Everything the server still sends before closing."""
    received = b""
    while True:
        chunk = connection.recv(RECEIVE_CHUNK)
        if not chunk:
            return received
        received += chunk


def truncating_divide(left, right):
    """Integer division truncating toward zero, as C does."""
    quotient = abs(left) // abs(right)
    return -quotient if (left < 0) != (right < 0) else quotient


def solve(status_line):
    """The answer line for 'cs230 STATUS A OP B'."""
    _, _, left, operator, right = status_line.split(" ")
    left, right = int(left), int(right)
    answers = {"+": left + right, "-": left - right, "*": left * right}
    answer = answers[operator] if operator in answers else truncating_divide(left, right)
    return "%s %d\n" % (PROTOCOL, answer)


def bad_hello(connection):
    """A HELLO with the wrong domain: the server must close without a byte."""
    connection.sendall(b"cs230 HELLO nobody@example.com\n")
    return drain(connection)


def cr_answer(connection, identification):
    """A correct answer terminated by CRLF: extra byte, so the server must close without BYE."""
    connection.sendall(("%s HELLO %s\n" % (PROTOCOL, identification)).encode())
    status, buffer = read_line(connection, b"")
    connection.sendall(solve(status).replace("\n", "\r\n").encode())
    return buffer + drain(connection)


def finish_session(connection, identification, pause):
    """Solves every problem after pausing on the first one; returns the BYE line."""
    connection.sendall(("%s HELLO %s\n" % (PROTOCOL, identification)).encode())
    buffer = b""
    line, buffer = read_line(connection, buffer)
    time.sleep(pause)
    while line is not None and " STATUS " in line:
        connection.sendall(solve(line).encode())
        line, buffer = read_line(connection, buffer)
    return (line or "").encode()


def main(argv):
    """Connects to 127.0.0.1:PORT and runs the requested behaviour."""
    parser = argparse.ArgumentParser(description="Raw math-speak peer for the real-server e2e test.")
    parser.add_argument("port", type=int)
    parser.add_argument("--identification", default="jdoe@umass.edu")
    parser.add_argument("--pause", type=float, default=1.0, help="seconds to wait after the first STATUS")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--bad-hello", action="store_true")
    mode.add_argument("--cr-answer", action="store_true")
    mode.add_argument("--finish", action="store_true")
    options = parser.parse_args(argv)
    connection = socket.create_connection(("127.0.0.1", options.port))
    connection.settimeout(30)
    try:
        if options.bad_hello:
            received = bad_hello(connection)
        elif options.cr_answer:
            received = cr_answer(connection, options.identification)
        else:
            received = finish_session(connection, options.identification, options.pause)
    finally:
        connection.close()
    sys.stdout.write(received.decode("ascii", errors="replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
