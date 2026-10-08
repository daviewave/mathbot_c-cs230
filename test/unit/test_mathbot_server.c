/* Unit tests for server/mathbot_server.c via the include trick: the real main is
 * renamed so every static function is reachable directly. */
#include <stdio.h>
#include <stdlib.h>

int server_main(int argc, char **argv);
#define main server_main
#include "../../server/mathbot_server.c"
#undef main
#include "check.h"

/* Hashes count bytes and returns the lowercase hex digest in a static buffer. */
static const char *sha256_hex_of(const unsigned char *data, size_t count) {
    static char hex[SHA256_HEX_LENGTH + 1];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    Sha256 hash;
    sha256_init(&hash);
    sha256_update(&hash, data, count);
    sha256_final(&hash, digest);
    hex_encode(digest, sizeof digest, hex);
    return hex;
}

/* Hashes a string. */
static const char *sha256_hex_of_text(const char *text) {
    return sha256_hex_of((const unsigned char *)text, strlen(text));
}

static void test_sha256_standard_vectors(void) {
    CHECK_EQ_STR(sha256_hex_of_text(""),
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ_STR(sha256_hex_of_text("abc"),
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ_STR(sha256_hex_of_text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

/* Padding boundaries: 55 bytes leave room for the length, 56 and 64 do not. */
static void test_sha256_padding_boundaries(void) {
    unsigned char data[64];
    memset(data, 'a', sizeof data);
    CHECK_EQ_STR(sha256_hex_of(data, 55),
                 "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK_EQ_STR(sha256_hex_of(data, 56),
                 "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK_EQ_STR(sha256_hex_of(data, 64),
                 "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

/* 1000 bytes cycling 0..255, fed in uneven pieces, must match one-shot hashing. */
static void test_sha256_incremental_updates(void) {
    unsigned char data[1000];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    char hex[SHA256_HEX_LENGTH + 1];
    Sha256 hash;
    size_t index;
    for (index = 0; index < sizeof data; index++) {
        data[index] = (unsigned char)(index % 256);
    }
    CHECK_EQ_STR(sha256_hex_of(data, sizeof data),
                 "a8af099bf2e878609558dbf69d8f88f4a31040a8cf84b549a0cfa912f12ffc3f");
    sha256_init(&hash);
    sha256_update(&hash, data, 1);
    sha256_update(&hash, data + 1, 62);
    sha256_update(&hash, data + 63, 129);
    sha256_update(&hash, data + 192, 0);
    sha256_update(&hash, data + 192, 808);
    sha256_final(&hash, digest);
    hex_encode(digest, sizeof digest, hex);
    CHECK_EQ_STR(hex, "a8af099bf2e878609558dbf69d8f88f4a31040a8cf84b549a0cfa912f12ffc3f");
}

static void test_hex_encode(void) {
    const unsigned char bytes[] = { 0x00, 0x0f, 0xf0, 0xff, 0xab };
    char hex[11];
    hex_encode(bytes, sizeof bytes, hex);
    CHECK_EQ_STR(hex, "000ff0ffab");
    hex_encode(bytes, 0, hex);
    CHECK_EQ_STR(hex, "");
}

/* The default (empty) secret yields sha256(id), what the mock and lib.sh compute. */
static void test_derive_flag(void) {
    char flag[FLAG_LENGTH + 1];
    derive_flag("", "jdoe@umass.edu", flag);
    CHECK_EQ_STR(flag, "123d06b1572ce2c01f7daa04b129e2701ff5b995291ccc8b56e0d479b2585447");
    CHECK_EQ_INT(strlen(flag), 64);
    derive_flag("s3cret", "jdoe@umass.edu", flag);
    CHECK_EQ_STR(flag, "683bd731d86c4622ed9407ef435386ff861837d0219cc62ea2b1e87592b15210");
}

/* xorshift32 is deterministic for a seed, never yields 0, and a zero seed is repaired. */
static void test_random_generator(void) {
    uint32_t first = 12345, second = 12345, zero = 0;
    int index;
    bool saw_zero = false;
    CHECK_EQ_INT(next_random(&first), next_random(&second));
    CHECK_EQ_INT(next_random(&first), next_random(&second));
    CHECK(next_random(&zero) != 0);
    for (index = 0; index < 100000; index++) {
        saw_zero = saw_zero || next_random(&first) == 0;
    }
    CHECK(!saw_zero);
    CHECK_EQ_INT(random_in_range(&first, 5, 5), 5);
    for (index = 0; index < 10000; index++) {
        long value = random_in_range(&first, -3, 3);
        if (value < -3 || value > 3) {
            CHECK(value >= -3 && value <= 3);
        }
    }
}

static void test_problem_count_range(void) {
    uint32_t state = 1;
    int index;
    long lowest = PROBLEMS_MAX, highest = PROBLEMS_MIN;
    for (index = 0; index < 100000; index++) {
        long count = problem_count(&state);
        if (count < lowest) {
            lowest = count;
        }
        if (count > highest) {
            highest = count;
        }
    }
    CHECK(lowest >= PROBLEMS_MIN);
    CHECK(highest <= PROBLEMS_MAX);
    CHECK_EQ_INT(lowest, PROBLEMS_MIN);
    CHECK_EQ_INT(highest, PROBLEMS_MAX);
}

/* Operands stay in -1000..1000, the operator is one of four, the divisor is never 0. */
static void test_make_problem(void) {
    uint32_t state = 99;
    int index;
    bool saw_division = false, saw_negative = false;
    for (index = 0; index < 100000; index++) {
        MathProblem problem = make_problem(&state);
        bool in_range = problem.left >= OPERAND_MIN && problem.left <= OPERAND_MAX &&
                        problem.right >= OPERAND_MIN && problem.right <= OPERAND_MAX;
        bool known_operator = strchr(OPERATORS, problem.operation) != NULL && problem.operation != '\0';
        if (!in_range || !known_operator || (problem.operation == '/' && problem.right == 0)) {
            CHECK(in_range);
            CHECK(known_operator);
            CHECK(!(problem.operation == '/' && problem.right == 0));
        }
        saw_division = saw_division || problem.operation == '/';
        saw_negative = saw_negative || problem.left < 0 || problem.right < 0;
    }
    CHECK(saw_division);
    CHECK(saw_negative);
}

/* Builds a problem literal. */
static MathProblem problem_of(long long left, char operation, long long right) {
    MathProblem problem;
    problem.left = left;
    problem.operation = operation;
    problem.right = right;
    return problem;
}

/* Evaluates left <operation> right. */
static long long answer_of(long long left, char operation, long long right) {
    MathProblem problem = problem_of(left, operation, right);
    return evaluate(&problem);
}

static void test_evaluate(void) {
    CHECK_EQ_INT(answer_of(505, '*', 700), 353500);
    CHECK_EQ_INT(answer_of(200, '/', 3), 66);
    CHECK_EQ_INT(answer_of(-7, '/', 2), -3);
    CHECK_EQ_INT(answer_of(7, '/', -2), -3);
    CHECK_EQ_INT(answer_of(-7, '/', -2), 3);
    CHECK_EQ_INT(answer_of(0, '/', -5), 0);
    CHECK_EQ_INT(answer_of(-1000, '*', -1000), 1000000);
    CHECK_EQ_INT(answer_of(-1000, '+', 1000), 0);
    CHECK_EQ_INT(answer_of(-1000, '-', 1000), -2000);
    CHECK_EQ_INT(answer_of(3, '-', -4), 7);
}

static void test_parse_hello(void) {
    char identification[MAX_LINE_LENGTH];
    CHECK(parse_hello("cs230 HELLO jdoe@umass.edu", identification, sizeof identification));
    CHECK_EQ_STR(identification, "jdoe@umass.edu");
    CHECK(parse_hello("cs230 HELLO JDoe@UMass.EDU", identification, sizeof identification));
    CHECK_EQ_STR(identification, "JDoe@UMass.EDU");
    CHECK(!parse_hello("cs230 HELLO ", identification, sizeof identification));
    CHECK(!parse_hello("cs230 HELLO", identification, sizeof identification));
    CHECK(!parse_hello("cs230 HELLO @umass.edu", identification, sizeof identification));
    CHECK(!parse_hello("cs230 HELLO jdoe@gmail.com", identification, sizeof identification));
    CHECK(!parse_hello("cs230 HELLO jdoe@umass.edu extra", identification, sizeof identification));
    CHECK(!parse_hello("cs230 HELLO jdoe@umass.edu ", identification, sizeof identification));
    CHECK(!parse_hello("cs230 HELLO jdoe@umass.edu\r", identification, sizeof identification));
    CHECK(!parse_hello("cs230 HELLO  jdoe@umass.edu", identification, sizeof identification));
    CHECK(!parse_hello("cs230 hello jdoe@umass.edu", identification, sizeof identification));
    CHECK(!parse_hello("HELLO jdoe@umass.edu", identification, sizeof identification));
    CHECK(!parse_hello("", identification, sizeof identification));
    CHECK(!parse_hello("cs230 HELLO jdoe@umass.edu", identification, 10));
}

static void test_is_correct_answer(void) {
    MathProblem problem = problem_of(200, '/', 3);
    CHECK(is_correct_answer(&problem, "cs230 66"));
    CHECK(!is_correct_answer(&problem, "cs230 67"));
    CHECK(!is_correct_answer(&problem, "cs230  66"));
    CHECK(!is_correct_answer(&problem, "cs230 66 "));
    CHECK(!is_correct_answer(&problem, "cs230 66\r"));
    CHECK(!is_correct_answer(&problem, "66"));
    CHECK(!is_correct_answer(&problem, "cs230 +66"));
    CHECK(!is_correct_answer(&problem, ""));
    problem = problem_of(-7, '/', 2);
    CHECK(is_correct_answer(&problem, "cs230 -3"));
    CHECK(!is_correct_answer(&problem, "cs230 -4"));
}

/* The too-small capacities are volatile so the compiler cannot prove the truncation at build time. */
static void test_format_lines(void) {
    char line[MAX_LINE_LENGTH];
    volatile size_t too_small_for_status = 5;
    volatile size_t too_small_for_bye = 20;
    MathProblem problem = problem_of(505, '*', 700);
    CHECK(format_status(&problem, line, sizeof line));
    CHECK_EQ_STR(line, "cs230 STATUS 505 * 700\n");
    problem = problem_of(-5, '-', -12);
    CHECK(format_status(&problem, line, sizeof line));
    CHECK_EQ_STR(line, "cs230 STATUS -5 - -12\n");
    CHECK(!format_status(&problem, line, too_small_for_status));
    CHECK(format_bye("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", line, sizeof line));
    CHECK_EQ_STR(line, "cs230 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef BYE\n");
    CHECK(!format_bye("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", line, too_small_for_bye));
}

static void test_parse_port(void) {
    unsigned short port = 1;
    CHECK(parse_port("27993", &port));
    CHECK_EQ_INT(port, 27993);
    CHECK(parse_port("0", &port) && port == 0);
    CHECK(parse_port("65535", &port) && port == 65535);
    CHECK(!parse_port("65536", &port));
    CHECK(!parse_port("-1", &port));
    CHECK(!parse_port("+1", &port));
    CHECK(!parse_port("80x", &port));
    CHECK(!parse_port(" 80", &port));
    CHECK(!parse_port("", &port));
    CHECK(!parse_port("99999999999999999999", &port));
}

static void test_parse_problem_override(void) {
    long count = 0;
    CHECK(parse_problem_override("400", &count));
    CHECK_EQ_INT(count, 400);
    CHECK(parse_problem_override("1", &count) && count == 1);
    CHECK(parse_problem_override("100000", &count) && count == MAX_PROBLEM_OVERRIDE);
    CHECK(!parse_problem_override("0", &count));
    CHECK(!parse_problem_override("100001", &count));
    CHECK(!parse_problem_override("-5", &count));
    CHECK(!parse_problem_override("4e2", &count));
    CHECK(!parse_problem_override("", &count));
}

/* Sets or clears an environment variable, failing the test on error. */
static void set_variable(const char *name, const char *value) {
    if (value == NULL) {
        CHECK(unsetenv(name) == 0);
    } else {
        CHECK(setenv(name, value, 1) == 0);
    }
}

/* Argument beats MATHBOT_PORT beats the default; MATHBOT_PROBLEMS and MATHBOT_SECRET are read once. */
static void test_load_config(void) {
    ServerConfig config;
    char *no_port[] = { "mathbot_server", NULL };
    char *with_port[] = { "mathbot_server", "4242", NULL };
    char *bad_port[] = { "mathbot_server", "99999", NULL };
    char *too_many[] = { "mathbot_server", "1", "2", NULL };
    set_variable(PORT_VARIABLE, NULL);
    set_variable(PROBLEMS_VARIABLE, NULL);
    set_variable(SECRET_VARIABLE, NULL);
    CHECK(load_config(1, no_port, &config));
    CHECK_EQ_INT(config.port, DEFAULT_PORT);
    CHECK_EQ_INT(config.problem_override, 0);
    CHECK_EQ_STR(config.secret, "");
    set_variable(PORT_VARIABLE, "5555");
    set_variable(PROBLEMS_VARIABLE, "400");
    set_variable(SECRET_VARIABLE, "s3cret");
    CHECK(load_config(1, no_port, &config));
    CHECK_EQ_INT(config.port, 5555);
    CHECK_EQ_INT(config.problem_override, 400);
    CHECK_EQ_STR(config.secret, "s3cret");
    CHECK(load_config(2, with_port, &config));
    CHECK_EQ_INT(config.port, 4242);
    CHECK(!load_config(2, bad_port, &config));
    CHECK(!load_config(3, too_many, &config));
    set_variable(PORT_VARIABLE, "abc");
    CHECK(!load_config(1, no_port, &config));
    set_variable(PORT_VARIABLE, "");
    set_variable(PROBLEMS_VARIABLE, "0");
    CHECK(!load_config(1, no_port, &config));
    set_variable(PROBLEMS_VARIABLE, NULL);
    CHECK(load_config(1, no_port, &config));
    CHECK_EQ_INT(config.port, DEFAULT_PORT);
    set_variable(PORT_VARIABLE, NULL);
    set_variable(SECRET_VARIABLE, NULL);
}

/* Writes all of text to a descriptor, failing the test on a short write. */
static void write_text(int fd, const char *text) {
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
}

/* Lines arrive whole whether the peer split them, coalesced them, or stopped early. */
static void test_receive_line_frames_across_boundaries(void) {
    int pair[2];
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    write_text(pair[1], "cs230 HEL");
    write_text(pair[1], "LO jdoe@umass.edu\ncs230 66\ncs2");
    CHECK_EQ_INT(receive_line(pair[0], &buffer, line, sizeof line), RECEIVE_LINE);
    CHECK_EQ_STR(line, "cs230 HELLO jdoe@umass.edu");
    CHECK_EQ_INT(receive_line(pair[0], &buffer, line, sizeof line), RECEIVE_LINE);
    CHECK_EQ_STR(line, "cs230 66");
    write_text(pair[1], "30 -3\r\n");
    CHECK_EQ_INT(receive_line(pair[0], &buffer, line, sizeof line), RECEIVE_LINE);
    CHECK_EQ_STR(line, "cs230 -3\r");
    write_text(pair[1], "partial");
    CHECK(close(pair[1]) == 0);
    CHECK_EQ_INT(receive_line(pair[0], &buffer, line, sizeof line), RECEIVE_EOF);
    CHECK(close(pair[0]) == 0);
}

/* A line that never ends is a protocol error once the buffer is full, not a hang. */
static void test_receive_line_rejects_overlong_line(void) {
    int pair[2];
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    char filler[RECEIVE_BUFFER_SIZE + 1];
    memset(filler, 'x', sizeof filler);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(write(pair[1], filler, sizeof filler) == (ssize_t)sizeof filler);
    CHECK_EQ_INT(receive_line(pair[0], &buffer, line, sizeof line), RECEIVE_ERROR);
    CHECK(close(pair[1]) == 0);
    CHECK(close(pair[0]) == 0);
}

/* With SO_RCVTIMEO set, a silent peer yields RECEIVE_TIMEOUT instead of blocking forever. */
static void test_receive_line_times_out(void) {
    int pair[2];
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    struct timeval timeout = { 0, 50000 };
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(set_receive_timeout(pair[0], timeout));
    write_text(pair[1], "no newline yet");
    CHECK_EQ_INT(receive_line(pair[0], &buffer, line, sizeof line), RECEIVE_TIMEOUT);
    CHECK(close(pair[1]) == 0);
    CHECK(close(pair[0]) == 0);
}

static void test_describe_outcome(void) {
    CHECK_EQ_STR(describe_outcome(SESSION_FLAG_SENT), "flag sent");
    CHECK_EQ_STR(describe_outcome(SESSION_BAD_HELLO), "closed: bad HELLO");
    CHECK_EQ_STR(describe_outcome(SESSION_WRONG_ANSWER), "closed: wrong answer");
    CHECK_EQ_STR(describe_outcome(SESSION_CLIENT_LEFT), "closed: client hung up");
    CHECK_EQ_STR(describe_outcome(SESSION_TIMED_OUT), "closed: timed out");
    CHECK_EQ_STR(describe_outcome(SESSION_FAILED), "closed: socket error");
}

/* The override wins; otherwise the count is the seeded random one in range. */
static void test_session_problem_count(void) {
    ServerConfig config = { 0, 400, "" };
    uint32_t state = 7;
    long count;
    CHECK_EQ_INT(session_problem_count(&config, &state), 400);
    config.problem_override = 0;
    count = session_problem_count(&config, &state);
    CHECK(count >= PROBLEMS_MIN && count <= PROBLEMS_MAX);
}

int main(void) {
    test_sha256_standard_vectors();
    test_sha256_padding_boundaries();
    test_sha256_incremental_updates();
    test_hex_encode();
    test_derive_flag();
    test_random_generator();
    test_problem_count_range();
    test_make_problem();
    test_evaluate();
    test_parse_hello();
    test_is_correct_answer();
    test_format_lines();
    test_parse_port();
    test_parse_problem_override();
    test_load_config();
    test_receive_line_frames_across_boundaries();
    test_receive_line_rejects_overlong_line();
    test_receive_line_times_out();
    test_describe_outcome();
    test_session_problem_count();
    CHECK_REPORT("test_mathbot_server");
}
