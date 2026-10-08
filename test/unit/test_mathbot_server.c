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
    sha256_digest(data, count, digest);
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
    CHECK_REPORT("test_mathbot_server");
}
