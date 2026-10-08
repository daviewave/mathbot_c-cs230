/* Unit tests for src/client.c via the include trick: the real main is renamed
 * so every static function is reachable directly. */
#include <stdio.h>
#include <stdlib.h>

int program_main(int argc, char **argv);
#define main program_main
#include "../../src/client.c"
#undef main
#include "check.h"

static void test_identification(void) {
    CHECK(is_valid_identification("jdoe@umass.edu"));
    CHECK(!is_valid_identification("@umass.edu"));
    CHECK(!is_valid_identification("jdoe@gmail.com"));
    CHECK(!is_valid_identification("jdoe@umass.edu "));
    CHECK(!is_valid_identification("j doe@umass.edu"));
    CHECK(!is_valid_identification("a@b@umass.edu"));
    CHECK(!is_valid_identification(""));
}

static void test_parse_port(void) {
    unsigned short port = 0;
    CHECK(parse_port("27993", &port));
    CHECK_EQ_INT(port, 27993);
    CHECK(parse_port("1", &port) && port == 1);
    CHECK(parse_port("65535", &port) && port == 65535);
    CHECK(!parse_port("0", &port));
    CHECK(!parse_port("65536", &port));
    CHECK(!parse_port("-1", &port));
    CHECK(!parse_port("+5", &port));
    CHECK(!parse_port("80x", &port));
    CHECK(!parse_port(" 80", &port));
    CHECK(!parse_port("", &port));
    CHECK(!parse_port("99999999999999999999", &port));
}

static void test_host(void) {
    CHECK(is_valid_host("128.119.243.147"));
    CHECK(is_valid_host("127.0.0.1"));
    CHECK(!is_valid_host("localhost"));
    CHECK(!is_valid_host("256.1.1.1"));
    CHECK(!is_valid_host("1.2.3"));
    CHECK(!is_valid_host(""));
}

static void test_parse_arguments(void) {
    char *good[] = { "client", "jdoe@umass.edu", "27993", "128.119.243.147", NULL };
    char *bad_count[] = { "client", "jdoe@umass.edu", "27993", NULL };
    char *bad_port[] = { "client", "jdoe@umass.edu", "port", "128.119.243.147", NULL };
    char *bad_host[] = { "client", "jdoe@umass.edu", "27993", "host", NULL };
    char *bad_id[] = { "client", "jdoe", "27993", "128.119.243.147", NULL };
    ClientArguments arguments;
    CHECK(parse_arguments(4, good, &arguments));
    CHECK_EQ_STR(arguments.identification, "jdoe@umass.edu");
    CHECK_EQ_INT(arguments.port, 27993);
    CHECK_EQ_STR(arguments.host, "128.119.243.147");
    CHECK(!parse_arguments(3, bad_count, &arguments));
    CHECK(!parse_arguments(4, bad_port, &arguments));
    CHECK(!parse_arguments(4, bad_host, &arguments));
    CHECK(!parse_arguments(4, bad_id, &arguments));
}

static void test_take_line_split_fragments(void) {
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    CHECK(line_buffer_append(&buffer, "cs230 STATUS 1", 14));
    CHECK(!line_buffer_take_line(&buffer, line, sizeof line));
    CHECK(line_buffer_append(&buffer, "2 + 3\n", 6));
    CHECK(line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_STR(line, "cs230 STATUS 12 + 3");
    CHECK(!line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_INT(buffer.used, 0);
}

static void test_take_line_coalesced(void) {
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    const char *two = "cs230 STATUS 1 + 1\ncs230 STATUS 2 * 2\ncs230 ST";
    CHECK(line_buffer_append(&buffer, two, strlen(two)));
    CHECK(line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_STR(line, "cs230 STATUS 1 + 1");
    CHECK(line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_STR(line, "cs230 STATUS 2 * 2");
    CHECK(!line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_INT(buffer.used, 8);
    CHECK(line_buffer_append(&buffer, "ATUS 3 - 4\n", 11));
    CHECK(line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_STR(line, "cs230 STATUS 3 - 4");
    CHECK_EQ_INT(buffer.used, 0);
}

static void test_take_line_empty_line(void) {
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    CHECK(line_buffer_append(&buffer, "\n", 1));
    CHECK(line_buffer_take_line(&buffer, line, sizeof line));
    CHECK_EQ_STR(line, "");
}

static void test_take_line_rejects_line_over_capacity(void) {
    LineBuffer buffer = { {0}, 0 };
    char small[4];
    CHECK(line_buffer_append(&buffer, "abcd\n", 5));
    CHECK(!line_buffer_take_line(&buffer, small, sizeof small));
    CHECK_EQ_INT(buffer.used, 5);
}

static void test_append_overflow(void) {
    LineBuffer buffer = { {0}, 0 };
    char filler[RECEIVE_BUFFER_SIZE];
    memset(filler, 'x', sizeof filler);
    CHECK(line_buffer_append(&buffer, filler, sizeof filler));
    CHECK(!line_buffer_append(&buffer, "y", 1));
    CHECK_EQ_INT(buffer.used, RECEIVE_BUFFER_SIZE);
}

static void test_receive_line_byte_by_byte(void) {
    int fds[2];
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    const char *message = "cs230 STATUS 12 + 34\n";
    size_t i;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    for (i = 0; i < strlen(message); i++) {
        CHECK(write(fds[1], message + i, 1) == 1);
    }
    CHECK_EQ_INT(receive_line(fds[0], &buffer, line, sizeof line), RECEIVE_LINE);
    CHECK_EQ_STR(line, "cs230 STATUS 12 + 34");
    CHECK_EQ_INT(buffer.used, 0);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

static void test_receive_line_coalesced_then_eof(void) {
    int fds[2];
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    const char *message = "cs230 STATUS 1 + 1\ncs230 STATUS 2 + 2\npartial";
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(write(fds[1], message, strlen(message)) == (ssize_t)strlen(message));
    CHECK(close(fds[1]) == 0);
    CHECK_EQ_INT(receive_line(fds[0], &buffer, line, sizeof line), RECEIVE_LINE);
    CHECK_EQ_STR(line, "cs230 STATUS 1 + 1");
    CHECK_EQ_INT(receive_line(fds[0], &buffer, line, sizeof line), RECEIVE_LINE);
    CHECK_EQ_STR(line, "cs230 STATUS 2 + 2");
    CHECK_EQ_INT(receive_line(fds[0], &buffer, line, sizeof line), RECEIVE_EOF);
    CHECK(close(fds[0]) == 0);
}

static void test_receive_line_too_long(void) {
    int fds[2];
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    char filler[RECEIVE_BUFFER_SIZE + 1];
    memset(filler, 'x', sizeof filler);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(write(fds[1], filler, sizeof filler) == (ssize_t)sizeof filler);
    CHECK_EQ_INT(receive_line(fds[0], &buffer, line, sizeof line), RECEIVE_ERROR);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

static void test_receive_line_closed_socket(void) {
    int fds[2];
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
    CHECK_EQ_INT(receive_line(fds[0], &buffer, line, sizeof line), RECEIVE_ERROR);
}

/* Reads whatever the peer wrote into received as a string. */
static void read_all_available(int fd, char *received, size_t capacity) {
    ssize_t count = read(fd, received, capacity - 1);
    received[count > 0 ? (size_t)count : 0] = '\0';
}

static void test_send_hello_exact_bytes(void) {
    int fds[2];
    char received[64];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(send_hello(fds[0], "jdoe@umass.edu", false));
    read_all_available(fds[1], received, sizeof received);
    CHECK_EQ_STR(received, "cs230 HELLO jdoe@umass.edu\n");
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

static void test_send_all_to_closed_peer_fails(void) {
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    CHECK(close(fds[1]) == 0);
    CHECK(!send_all(fds[0], "x\n", 2));
    CHECK(close(fds[0]) == 0);
}

static void test_connect_refused(void) {
    CHECK(connect_to_server("127.0.0.1", 1) < 0);
}

static void test_verbose_flag(void) {
    CHECK(setenv(VERBOSE_VARIABLE, "1", 1) == 0);
    CHECK(is_verbose_enabled());
    CHECK(setenv(VERBOSE_VARIABLE, "0", 1) == 0);
    CHECK(!is_verbose_enabled());
    CHECK(unsetenv(VERBOSE_VARIABLE) == 0);
    CHECK(!is_verbose_enabled());
}

static void test_parse_status_accepts_spec_example(void) {
    MathProblem problem;
    CHECK(parse_status("cs230 STATUS 505 * 700", &problem));
    CHECK_EQ_INT(problem.left, 505);
    CHECK_EQ_INT(problem.operation, '*');
    CHECK_EQ_INT(problem.right, 700);
}

static void test_parse_status_accepts_negatives(void) {
    MathProblem problem;
    CHECK(parse_status("cs230 STATUS -12 / -5", &problem));
    CHECK_EQ_INT(problem.left, -12);
    CHECK_EQ_INT(problem.operation, '/');
    CHECK_EQ_INT(problem.right, -5);
    CHECK(parse_status("cs230 STATUS 0 - 0", &problem));
    CHECK_EQ_INT(problem.left, 0);
    CHECK_EQ_INT(problem.right, 0);
}

static void test_parse_status_rejects_malformed(void) {
    MathProblem problem;
    CHECK(!parse_status("cs230 STATUS 1  + 2", &problem));
    CHECK(!parse_status("cs230 STATUS  1 + 2", &problem));
    CHECK(!parse_status("cs230 STATUS 1 + 2 ", &problem));
    CHECK(!parse_status("cs230 STATUS 1 + 2 3", &problem));
    CHECK(!parse_status("cs230 STATUS 1 % 2", &problem));
    CHECK(!parse_status("cs230 STATUS 1 +", &problem));
    CHECK(!parse_status("cs230 STATUS 1 + ", &problem));
    CHECK(!parse_status("cs230 STATUS a + 2", &problem));
    CHECK(!parse_status("cs230 STATUS +1 + 2", &problem));
    CHECK(!parse_status("cs230 STATUS - + 2", &problem));
    CHECK(!parse_status("cs230 STATUS 1 + 2\r", &problem));
    CHECK(!parse_status("cs230 STATUS 99999999999999999999 + 2", &problem));
    CHECK(!parse_status("cs230 STATUS 1 + -9999999999999999999", &problem));
    CHECK(!parse_status("cs230 HELLO 1 + 2", &problem));
    CHECK(!parse_status("cs230 STATUS", &problem));
    CHECK(!parse_status("", &problem));
}

static void test_parse_bye(void) {
    char flag[FLAG_CAPACITY];
    CHECK(parse_bye("cs230 7c5ee45183d657f5148fd4bbabb6615128ec32699164980be7b8b451fd9ac0c3 BYE", flag, sizeof flag));
    CHECK_EQ_STR(flag, "7c5ee45183d657f5148fd4bbabb6615128ec32699164980be7b8b451fd9ac0c3");
    CHECK(parse_bye("cs230 x BYE", flag, sizeof flag));
    CHECK_EQ_STR(flag, "x");
    CHECK(!parse_bye("cs230  BYE", flag, sizeof flag));
    CHECK(!parse_bye("cs230 BYE", flag, sizeof flag));
    CHECK(!parse_bye("cs230 abc def BYE", flag, sizeof flag));
    CHECK(!parse_bye("cs230 abc BYE ", flag, sizeof flag));
    CHECK(!parse_bye("cs230 abc BYE\r", flag, sizeof flag));
    CHECK(!parse_bye("cs230 STATUS 1 + 2", flag, sizeof flag));
    CHECK(!parse_bye("cs230 abc", flag, sizeof flag));
    CHECK(!parse_bye("", flag, sizeof flag));
    CHECK(!parse_bye("cs230 abcdef BYE", flag, 4));
}

static void test_evaluate_basic(void) {
    long long result;
    MathProblem add = { 505, '+', 700 };
    MathProblem sub = { 5, '-', 9 };
    MathProblem mul = { 505, '*', 700 };
    MathProblem div = { 200, '/', 3 };
    CHECK(evaluate(&add, &result) && result == 1205);
    CHECK(evaluate(&sub, &result) && result == -4);
    CHECK(evaluate(&mul, &result) && result == 353500);
    CHECK(evaluate(&div, &result) && result == 66);
}

static void test_evaluate_division_truncates_toward_zero(void) {
    long long result;
    MathProblem a = { -7, '/', 2 };
    MathProblem b = { 7, '/', -2 };
    MathProblem c = { -7, '/', -2 };
    MathProblem d = { 1, '/', 3 };
    MathProblem e = { -200, '/', 3 };
    CHECK(evaluate(&a, &result) && result == -3);
    CHECK(evaluate(&b, &result) && result == -3);
    CHECK(evaluate(&c, &result) && result == 3);
    CHECK(evaluate(&d, &result) && result == 0);
    CHECK(evaluate(&e, &result) && result == -66);
}

static void test_evaluate_division_by_zero_refused(void) {
    long long result;
    MathProblem zero = { 5, '/', 0 };
    MathProblem min = { LLONG_MIN, '/', -1 };
    MathProblem min_ok = { LLONG_MIN, '/', 1 };
    CHECK(!evaluate(&zero, &result));
    CHECK(!evaluate(&min, &result));
    CHECK(evaluate(&min_ok, &result) && result == LLONG_MIN);
}

static void test_evaluate_overflow_refused(void) {
    long long result;
    MathProblem add = { LLONG_MAX, '+', 1 };
    MathProblem add_neg = { LLONG_MIN, '+', -1 };
    MathProblem add_ok = { LLONG_MAX, '+', -1 };
    MathProblem sub = { LLONG_MIN, '-', 1 };
    MathProblem sub_neg = { LLONG_MAX, '-', -1 };
    MathProblem sub_ok = { LLONG_MIN, '-', -1 };
    MathProblem mul = { LLONG_MAX, '*', 2 };
    MathProblem mul_neg = { LLONG_MIN, '*', -1 };
    MathProblem mul_mixed = { -3037000500LL, '*', 3037000500LL };
    MathProblem mul_both_neg = { -3037000500LL, '*', -3037000500LL };
    MathProblem mul_ok = { -3037000499LL, '*', 3037000499LL };
    MathProblem mul_zero = { LLONG_MIN, '*', 0 };
    MathProblem unknown = { 1, '%', 1 };
    CHECK(!evaluate(&add, &result));
    CHECK(!evaluate(&add_neg, &result));
    CHECK(evaluate(&add_ok, &result) && result == LLONG_MAX - 1);
    CHECK(!evaluate(&sub, &result));
    CHECK(!evaluate(&sub_neg, &result));
    CHECK(evaluate(&sub_ok, &result) && result == LLONG_MIN + 1);
    CHECK(!evaluate(&mul, &result));
    CHECK(!evaluate(&mul_neg, &result));
    CHECK(!evaluate(&mul_mixed, &result));
    CHECK(!evaluate(&mul_both_neg, &result));
    CHECK(evaluate(&mul_ok, &result) && result == -9223372030926249001LL);
    CHECK(evaluate(&mul_zero, &result) && result == 0);
    CHECK(!evaluate(&unknown, &result));
}

static void test_handle_status_exact_bytes(void) {
    int fds[2];
    char received[64];
    MathProblem problem = { 505, '*', 700 };
    MathProblem negative = { -200, '/', 3 };
    MathProblem bad = { 1, '/', 0 };
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(handle_status(fds[0], &problem, false));
    read_all_available(fds[1], received, sizeof received);
    CHECK_EQ_STR(received, "cs230 353500\n");
    CHECK(handle_status(fds[0], &negative, false));
    read_all_available(fds[1], received, sizeof received);
    CHECK_EQ_STR(received, "cs230 -66\n");
    CHECK(!handle_status(fds[0], &bad, false));
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

/* Runs run_session against a scripted peer that writes script then closes; returns the status. */
static int run_scripted_session(const char *script, char *received, size_t capacity) {
    int fds[2];
    int status;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(write(fds[1], script, strlen(script)) == (ssize_t)strlen(script));
    CHECK(shutdown(fds[1], SHUT_WR) == 0);
    status = run_session(fds[0], "jdoe@umass.edu", false);
    read_all_available(fds[1], received, capacity);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
    return status;
}

static void test_run_session_answers_then_prints_flag(void) {
    char received[256];
    CHECK_EQ_INT(run_scripted_session("cs230 STATUS 505 * 700\ncs230 STATUS 200 / 3\ncs230 abc123 BYE\n",
                                      received, sizeof received), EXIT_SUCCESS);
    CHECK_EQ_STR(received, "cs230 HELLO jdoe@umass.edu\ncs230 353500\ncs230 66\n");
}

static void test_run_session_early_close_fails(void) {
    char received[256];
    CHECK_EQ_INT(run_scripted_session("cs230 STATUS 1 + 1\n", received, sizeof received), EXIT_FAILURE);
    CHECK_EQ_STR(received, "cs230 HELLO jdoe@umass.edu\ncs230 2\n");
}

static void test_run_session_rejects_garbage(void) {
    char received[256];
    CHECK_EQ_INT(run_scripted_session("cs230 WHAT\ncs230 STATUS 1 + 1\n", received, sizeof received), EXIT_FAILURE);
    CHECK_EQ_STR(received, "cs230 HELLO jdoe@umass.edu\n");
    CHECK_EQ_INT(run_scripted_session("cs230 STATUS 1 / 0\n", received, sizeof received), EXIT_FAILURE);
    CHECK_EQ_STR(received, "cs230 HELLO jdoe@umass.edu\n");
}

int main(void) {
    test_identification();
    test_parse_port();
    test_host();
    test_parse_arguments();
    test_take_line_split_fragments();
    test_take_line_coalesced();
    test_take_line_empty_line();
    test_take_line_rejects_line_over_capacity();
    test_append_overflow();
    test_receive_line_byte_by_byte();
    test_receive_line_coalesced_then_eof();
    test_receive_line_too_long();
    test_receive_line_closed_socket();
    test_send_hello_exact_bytes();
    test_send_all_to_closed_peer_fails();
    test_connect_refused();
    test_verbose_flag();
    test_parse_status_accepts_spec_example();
    test_parse_status_accepts_negatives();
    test_parse_status_rejects_malformed();
    test_parse_bye();
    test_evaluate_basic();
    test_evaluate_division_truncates_toward_zero();
    test_evaluate_division_by_zero_refused();
    test_evaluate_overflow_refused();
    test_handle_status_exact_bytes();
    test_run_session_answers_then_prints_flag();
    test_run_session_early_close_fails();
    test_run_session_rejects_garbage();
    CHECK_REPORT("test_client");
}
