#!/usr/bin/env python3
"""Mock math-speak server for the e2e suite: serves exactly one connection on a loopback port, then exits.

Exit status is 0 only when the flag was sent (or, with --inject, once the client reacted to the
injected line). See docs/design.md section 8 for why each option exists.
"""
import argparse
import hashlib
import os
import random
import socket
import sys
import time

PROTOCOL = "cs230"
HELLO_PREFIX = PROTOCOL + " HELLO "
OPERATORS = "+-*/"
OPERAND_MIN = -1000
OPERAND_MAX = 1000
PROBLEMS_MIN = 300
PROBLEMS_MAX = 2000
FRAGMENT_EVERY = 7
PIPELINE_EVERY = 5
FRAGMENT_PAUSE_SECONDS = 0.002
SOCKET_TIMEOUT_SECONDS = 30.0
RECEIVE_CHUNK = 4096


class ProtocolFailure(Exception):
    """Raised when the client deviates from math speak or the connection dies early."""


class LineReader:
    """Frames newline-terminated lines out of a stream socket, whatever the recv boundaries are."""

    def __init__(self, connection):
        """Wraps an accepted connection with an empty buffer."""
        self.connection = connection
        self.buffer = b""

    def read_line(self):
        """Returns the next line without its newline, or None once the peer has closed."""
        while b"\n" not in self.buffer:
            chunk = self._receive_chunk()
            if not chunk:
                return None
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b"\n")
        return line.decode("utf-8", errors="replace")

    def _receive_chunk(self):
        """Reads one chunk, translating a timeout or a reset into end of stream."""
        try:
            acknowledge_immediately(self.connection)
            return self.connection.recv(RECEIVE_CHUNK)
        except socket.timeout:
            raise ProtocolFailure("timed out waiting for the client")
        except ConnectionError:
            return b""


class Sender:
    """Sends outgoing messages, fragmenting every FRAGMENT_EVERY-th one when asked to."""

    def __init__(self, connection, fragment, rng, crlf=False):
        """Remembers the connection, the fragmentation switch, the rng for split points and the line ending."""
        self.connection = connection
        self.fragment = fragment
        self.rng = rng
        self.crlf = crlf
        self.sent_messages = 0

    def send(self, data, force_fragment=False):
        """Sends one message, split in two writes when this is a fragmenting turn."""
        self.sent_messages += 1
        if self.crlf:
            data = data.replace(b"\n", b"\r\n")
        fragment_turn = self.fragment and self.sent_messages % FRAGMENT_EVERY == 0
        try:
            if (fragment_turn or force_fragment) and len(data) > 1:
                self._send_fragmented(data)
            else:
                self.connection.sendall(data)
        except (ConnectionError, socket.timeout):
            raise ProtocolFailure("connection lost while sending")

    def _send_fragmented(self, data):
        """Writes the message as two pieces split at a random byte, with a short pause between."""
        split = self.rng.randint(1, len(data) - 1)
        self.connection.sendall(data[:split])
        time.sleep(FRAGMENT_PAUSE_SECONDS)
        self.connection.sendall(data[split:])


def parse_arguments(argv):
    """Parses the command line described in docs/plan.md Task 6."""
    parser = argparse.ArgumentParser(description="Mock math-speak server for the Math Bot e2e suite.")
    parser.add_argument("--port-file", required=True, help="file that receives the chosen port")
    parser.add_argument("--seed", type=int, default=1, help="seed for the problem generator")
    parser.add_argument("--problems", type=int, default=None, help="override the seeded problem count")
    parser.add_argument("--fragment", action="store_true", help="split every 7th message across two writes")
    parser.add_argument("--pipeline", action="store_true", help="send every 5th problem together with the next")
    parser.add_argument("--reject", action="store_true", help="close after the first answer without BYE")
    parser.add_argument("--inject", default=None, metavar="LINE", help="send LINE instead of the first problem")
    parser.add_argument("--identification", default=None, metavar="ID",
                        help="require the HELLO line to carry exactly this identification")
    parser.add_argument("--crlf", action="store_true", help="terminate every server line with CRLF")
    return parser.parse_args(argv)


def listen_on_loopback():
    """Binds a listening TCP socket to 127.0.0.1 on an ephemeral port."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(SOCKET_TIMEOUT_SECONDS)
    return listener


def publish_port(path, port):
    """Writes the port number to path atomically so a reader never sees a partial file."""
    temporary = "%s.%d.tmp" % (path, os.getpid())
    with open(temporary, "w") as handle:
        handle.write("%d\n" % port)
    os.replace(temporary, path)


def acknowledge_immediately(connection):
    """Disables delayed ACK where supported so a pipelining client's second answer is not held by Nagle."""
    if hasattr(socket, "TCP_QUICKACK"):
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 1)


def accept_one(listener):
    """Accepts a single client, sets TCP_NODELAY and the overall timeout, closes the listener."""
    try:
        connection, _ = listener.accept()
    except socket.timeout:
        raise ProtocolFailure("no client connected in time")
    finally:
        listener.close()
    connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    connection.settimeout(SOCKET_TIMEOUT_SECONDS)
    return connection


def expect_hello(reader, required_identification):
    """Requires the first line to be exactly 'cs230 HELLO <id>' and returns the identification."""
    line = reader.read_line()
    if line is None:
        raise ProtocolFailure("client closed before HELLO")
    if not line.startswith(HELLO_PREFIX):
        raise ProtocolFailure("expected HELLO, got %r" % line)
    identification = line[len(HELLO_PREFIX):]
    if not identification or identification != identification.strip() or " " in identification:
        raise ProtocolFailure("malformed identification %r" % identification)
    if required_identification is not None and identification != required_identification:
        raise ProtocolFailure("HELLO carried %r, expected %r" % (identification, required_identification))
    return identification


def flag_for(identification):
    """The 64-hex flag the client must capture: sha256 of the identification."""
    return hashlib.sha256(identification.encode()).hexdigest()


def truncating_divide(left, right):
    """Integer division truncating toward zero, as C does; never Python's floor division."""
    quotient = abs(left) // abs(right)
    return -quotient if (left < 0) != (right < 0) else quotient


def evaluate(left, operator, right):
    """The answer the client must send for 'left operator right'."""
    if operator == "+":
        return left + right
    if operator == "-":
        return left - right
    if operator == "*":
        return left * right
    return truncating_divide(left, right)


def make_problem(rng):
    """Draws one problem with operands in OPERAND_MIN..OPERAND_MAX and a non-zero divisor."""
    left = rng.randint(OPERAND_MIN, OPERAND_MAX)
    operator = rng.choice(OPERATORS)
    right = rng.randint(OPERAND_MIN, OPERAND_MAX)
    while operator == "/" and right == 0:
        right = rng.randint(OPERAND_MIN, OPERAND_MAX)
    return left, operator, right


def format_status(problem):
    """Encodes a problem as the STATUS line the spec prescribes."""
    left, operator, right = problem
    return ("%s STATUS %d %s %d\n" % (PROTOCOL, left, operator, right)).encode()


def expect_answer(reader, problem):
    """Reads one line and requires it to be byte-exactly 'cs230 <answer>'."""
    expected = "%s %d" % (PROTOCOL, evaluate(*problem))
    line = reader.read_line()
    if line is None:
        raise ProtocolFailure("client closed before answering %r" % format_status(problem))
    if line != expected:
        raise ProtocolFailure("wrong answer %r to %r (expected %r)" % (line, format_status(problem), expected))


def problem_count(options):
    """The seeded random count in 300..2000 unless --problems overrides it."""
    if options.problems is not None:
        return options.problems
    return random.Random(options.seed).randint(PROBLEMS_MIN, PROBLEMS_MAX)


def problem_batches(problems, pipeline):
    """Groups problems into send batches: pairs at every PIPELINE_EVERY-th problem when pipelining."""
    batches = []
    index = 0
    while index < len(problems):
        paired = pipeline and (index + 1) % PIPELINE_EVERY == 0 and index + 1 < len(problems)
        batches.append(problems[index:index + 2] if paired else problems[index:index + 1])
        index += len(batches[-1])
    return batches


def serve_problems(sender, reader, problems, options):
    """Sends every problem batch and checks every answer; closes on the first wrong one."""
    for batch in problem_batches(problems, options.pipeline):
        sender.send(b"".join(format_status(problem) for problem in batch))
        for problem in batch:
            expect_answer(reader, problem)
        if options.reject:
            raise ProtocolFailure("rejecting the client after its first answer, as asked")


def serve_injected_line(sender, reader, line):
    """Sends the injected line first, then waits for the client to answer or hang up."""
    sender.send((line + "\n").encode())
    reader.read_line()


def send_bye(sender, identification, fragment):
    """Sends the flag line, always fragmented when fragmentation is on."""
    sender.send(("%s %s BYE\n" % (PROTOCOL, flag_for(identification))).encode(), force_fragment=fragment)


def serve(connection, options):
    """Runs one math-speak session; returns normally only when the session reached its planned end."""
    reader = LineReader(connection)
    sender = Sender(connection, options.fragment, random.Random(options.seed ^ 0x5EED), options.crlf)
    identification = expect_hello(reader, options.identification)
    if options.inject is not None:
        serve_injected_line(sender, reader, options.inject)
        return
    rng = random.Random(options.seed)
    problems = [make_problem(rng) for _ in range(problem_count(options))]
    serve_problems(sender, reader, problems, options)
    send_bye(sender, identification, options.fragment)


def main(argv):
    """Entry point: listen, publish the port, serve one client, report the outcome in the exit status."""
    options = parse_arguments(argv)
    listener = listen_on_loopback()
    publish_port(options.port_file, listener.getsockname()[1])
    connection = None
    try:
        connection = accept_one(listener)
        serve(connection, options)
        return 0
    except ProtocolFailure as failure:
        print("mock_server: %s" % failure, file=sys.stderr)
        return 1
    finally:
        if connection is not None:
            connection.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
