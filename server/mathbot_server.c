/* mathbot_server.c: a local stand-in for the CS230 math-speak server. Accepts
 * connections forever, forks one child per session, sends 300..2000 arithmetic
 * problems and ends a fully correct session with a SHA-256 flag. Design notes
 * live in docs/server.md. */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    DEFAULT_PORT = 27993,
    MAX_PORT = 65535,
    MAX_ARGUMENTS = 1,
    LISTEN_BACKLOG = 16,
    RECEIVE_BUFFER_SIZE = 4096,
    SESSION_TIMEOUT_SECONDS = 30,
    ACCEPT_POLL_SECONDS = 1,
    MAX_PROBLEM_OVERRIDE = 100000,
    PEER_TEXT_CAPACITY = INET_ADDRSTRLEN + 6,
    PROBLEMS_MIN = 300,
    PROBLEMS_MAX = 2000,
    OPERAND_MIN = -1000,
    OPERAND_MAX = 1000,
    MAX_LINE_LENGTH = 512,
    SHA256_BLOCK_LENGTH = 64,
    SHA256_DIGEST_LENGTH = 32,
    SHA256_HEX_LENGTH = 64,
    SHA256_ROUNDS = 64,
    SHA256_STATE_WORDS = 8,
    SHA256_SCHEDULE_WORDS = 64,
    SHA256_LENGTH_FIELD = 8,
    FLAG_LENGTH = SHA256_HEX_LENGTH
};

#define PORT_VARIABLE "MATHBOT_PORT"
#define PROBLEMS_VARIABLE "MATHBOT_PROBLEMS"
#define SECRET_VARIABLE "MATHBOT_SECRET"
#define SEED_VARIABLE "MATHBOT_SEED"
#define OPERATORS "+-*/"
#define IDENTIFICATION_DOMAIN "@umass.edu"
#define PROTOCOL_PREFIX "cs230 "
#define HELLO_PREFIX "cs230 HELLO "
#define STATUS_PREFIX "cs230 STATUS "
#define BYE_SUFFIX " BYE"

/* Settings read once at startup and handed to every session; never global.
 * seed_override is 0 when MATHBOT_SEED is unset and every session seeds itself. */
typedef struct {
    unsigned short port;
    long problem_override;
    uint32_t seed_override;
    const char *secret;
} ServerConfig;

/* One arithmetic problem: left <operation> right. */
typedef struct {
    long long left;
    char operation;
    long long right;
} MathProblem;

/* Bytes received from the client that have not been consumed as whole lines yet. */
typedef struct {
    char data[RECEIVE_BUFFER_SIZE];
    size_t used;
} LineBuffer;

/* Outcome of waiting for one line; RECEIVE_PROTOCOL_ERROR is a line too long or holding a NUL. */
typedef enum {
    RECEIVE_LINE,
    RECEIVE_EOF,
    RECEIVE_TIMEOUT,
    RECEIVE_PROTOCOL_ERROR,
    RECEIVE_ERROR
} ReceiveResult;

/* What line_buffer_take_line found. */
typedef enum {
    TAKE_LINE,
    TAKE_INCOMPLETE,
    TAKE_MALFORMED
} TakeResult;

/* How a session step ended; everything but SESSION_OK stops the session. */
typedef enum {
    SESSION_OK,
    SESSION_FLAG_SENT,
    SESSION_BAD_HELLO,
    SESSION_WRONG_ANSWER,
    SESSION_CLIENT_LEFT,
    SESSION_TIMED_OUT,
    SESSION_MALFORMED_LINE,
    SESSION_FAILED
} SessionOutcome;

/* The only global: set by the SIGINT/SIGTERM handler, polled by the accept loop. */
static volatile sig_atomic_t shutdown_requested = 0;

/* SHA-256 running state: the eight hash words, bytes seen, and the partial block. */
typedef struct {
    uint32_t state[SHA256_STATE_WORDS];
    uint64_t byte_length;
    unsigned char block[SHA256_BLOCK_LENGTH];
    size_t block_used;
} Sha256;

/* ---- SHA-256 (FIPS 180-4, section 6.2) ---------------------------------- */

/* Rotates a 32-bit word right by count bits (FIPS ROTR). */
static uint32_t rotate_right(uint32_t word, unsigned int count) {
    return (word >> count) | (word << (32 - count));
}

/* FIPS Ch(x, y, z): bits chosen from y where x is set, from z elsewhere. */
static uint32_t sha256_choose(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

/* FIPS Maj(x, y, z): the majority bit of the three inputs. */
static uint32_t sha256_majority(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

/* FIPS big Sigma 0, used on the working variable a. */
static uint32_t sha256_big_sigma0(uint32_t x) {
    return rotate_right(x, 2) ^ rotate_right(x, 13) ^ rotate_right(x, 22);
}

/* FIPS big Sigma 1, used on the working variable e. */
static uint32_t sha256_big_sigma1(uint32_t x) {
    return rotate_right(x, 6) ^ rotate_right(x, 11) ^ rotate_right(x, 25);
}

/* FIPS small sigma 0, used in the message schedule. */
static uint32_t sha256_small_sigma0(uint32_t x) {
    return rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3);
}

/* FIPS small sigma 1, used in the message schedule. */
static uint32_t sha256_small_sigma1(uint32_t x) {
    return rotate_right(x, 17) ^ rotate_right(x, 19) ^ (x >> 10);
}

/* Reads one big-endian 32-bit word from bytes. */
static uint32_t load_big_endian_word(const unsigned char *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/* Writes one 32-bit word to bytes in big-endian order. */
static void store_big_endian_word(uint32_t word, unsigned char *bytes) {
    bytes[0] = (unsigned char)(word >> 24);
    bytes[1] = (unsigned char)(word >> 16);
    bytes[2] = (unsigned char)(word >> 8);
    bytes[3] = (unsigned char)word;
}

/* Writes a 64-bit length to bytes in big-endian order (the padding's final field). */
static void store_big_endian_length(uint64_t length, unsigned char *bytes) {
    store_big_endian_word((uint32_t)(length >> 32), bytes);
    store_big_endian_word((uint32_t)length, bytes + 4);
}

/* Expands one 64-byte block into the 64-word message schedule W. */
static void sha256_prepare_schedule(const unsigned char *block, uint32_t *schedule) {
    size_t index;
    for (index = 0; index < 16; index++) {
        schedule[index] = load_big_endian_word(block + index * 4);
    }
    for (index = 16; index < SHA256_SCHEDULE_WORDS; index++) {
        schedule[index] = sha256_small_sigma1(schedule[index - 2]) + schedule[index - 7] +
                          sha256_small_sigma0(schedule[index - 15]) + schedule[index - 16];
    }
}

/* Runs the 64 rounds over one block and adds the result into state. */
static void sha256_compress(uint32_t *state, const unsigned char *block) {
    static const uint32_t round_constants[SHA256_ROUNDS] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t schedule[SHA256_SCHEDULE_WORDS];
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    size_t round;
    sha256_prepare_schedule(block, schedule);
    for (round = 0; round < SHA256_ROUNDS; round++) {
        uint32_t temp1 = h + sha256_big_sigma1(e) + sha256_choose(e, f, g) +
                         round_constants[round] + schedule[round];
        uint32_t temp2 = sha256_big_sigma0(a) + sha256_majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/* Starts a hash with the FIPS initial hash value H(0). */
static void sha256_init(Sha256 *hash) {
    static const uint32_t initial_state[SHA256_STATE_WORDS] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(hash->state, initial_state, sizeof hash->state);
    hash->byte_length = 0;
    hash->block_used = 0;
}

/* Appends bytes to the partial block, compressing every time it fills. */
static void sha256_update(Sha256 *hash, const unsigned char *data, size_t count) {
    hash->byte_length += count;
    while (count > 0) {
        size_t room = SHA256_BLOCK_LENGTH - hash->block_used;
        size_t take = count < room ? count : room;
        memcpy(hash->block + hash->block_used, data, take);
        hash->block_used += take;
        data += take;
        count -= take;
        if (hash->block_used == SHA256_BLOCK_LENGTH) {
            sha256_compress(hash->state, hash->block);
            hash->block_used = 0;
        }
    }
}

/* Pads with 0x80, zeros and the bit length, compresses, and writes the digest. */
static void sha256_final(Sha256 *hash, unsigned char *digest) {
    uint64_t bit_length = hash->byte_length * 8;
    size_t index;
    hash->block[hash->block_used++] = 0x80;
    if (hash->block_used > SHA256_BLOCK_LENGTH - SHA256_LENGTH_FIELD) {
        memset(hash->block + hash->block_used, 0, SHA256_BLOCK_LENGTH - hash->block_used);
        sha256_compress(hash->state, hash->block);
        hash->block_used = 0;
    }
    memset(hash->block + hash->block_used, 0, SHA256_BLOCK_LENGTH - hash->block_used);
    store_big_endian_length(bit_length, hash->block + SHA256_BLOCK_LENGTH - SHA256_LENGTH_FIELD);
    sha256_compress(hash->state, hash->block);
    for (index = 0; index < SHA256_STATE_WORDS; index++) {
        store_big_endian_word(hash->state[index], digest + index * 4);
    }
}

/* Writes count bytes as 2*count lowercase hex digits plus a terminator into hex. */
static void hex_encode(const unsigned char *bytes, size_t count, char *hex) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0; index < count; index++) {
        hex[index * 2] = digits[bytes[index] >> 4];
        hex[index * 2 + 1] = digits[bytes[index] & 0x0f];
    }
    hex[count * 2] = '\0';
}

/* The flag for an identification: hex SHA-256 of secret followed by the id,
 * so an empty secret gives sha256(id), which the e2e helpers already compute. */
static void derive_flag(const char *secret, const char *identification, char *flag) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    Sha256 hash;
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char *)secret, strlen(secret));
    sha256_update(&hash, (const unsigned char *)identification, strlen(identification));
    sha256_final(&hash, digest);
    hex_encode(digest, sizeof digest, flag);
}

/* ---- Problems and math speak ------------------------------------------- */

/* Advances an xorshift32 generator and returns the next non-zero value. */
static uint32_t next_random(uint32_t *state) {
    uint32_t value = *state == 0 ? 1 : *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

/* A pseudo-random value in low..high inclusive (low <= high). */
static long random_in_range(uint32_t *state, long low, long high) {
    uint32_t span = (uint32_t)(high - low + 1);
    return low + (long)(next_random(state) % span);
}

/* How many problems this session asks: the spec's "no less than 300, no more than 2000". */
static long problem_count(uint32_t *state) {
    return random_in_range(state, PROBLEMS_MIN, PROBLEMS_MAX);
}

/* Draws one problem with operands in OPERAND_MIN..OPERAND_MAX and a non-zero divisor. */
static MathProblem make_problem(uint32_t *state) {
    MathProblem problem;
    problem.left = random_in_range(state, OPERAND_MIN, OPERAND_MAX);
    problem.operation = OPERATORS[random_in_range(state, 0, (long)strlen(OPERATORS) - 1)];
    problem.right = random_in_range(state, OPERAND_MIN, OPERAND_MAX);
    while (problem.operation == '/' && problem.right == 0) {
        problem.right = random_in_range(state, OPERAND_MIN, OPERAND_MAX);
    }
    return problem;
}

/* The answer the client must send; division truncates toward zero as C99 6.5.5
 * prescribes. Operands come from make_problem, so no overflow and no zero divisor. */
static long long evaluate(const MathProblem *problem) {
    switch (problem->operation) {
    case '+':
        return problem->left + problem->right;
    case '-':
        return problem->left - problem->right;
    case '*':
        return problem->left * problem->right;
    default:
        return problem->left / problem->right;
    }
}

/* True when left and right are equal ignoring ASCII case. */
static bool equals_ignoring_case(const char *left, const char *right) {
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
    }
    return *left == *right;
}

/* True when any character of text is whitespace. */
static bool contains_whitespace(const char *text) {
    for (; *text != '\0'; text++) {
        if (isspace((unsigned char)*text)) {
            return true;
        }
    }
    return false;
}

/* True for a non-empty token without whitespace ending in @umass.edu (any case). */
static bool is_valid_identification(const char *identification) {
    size_t length = strlen(identification);
    size_t domain_length = strlen(IDENTIFICATION_DOMAIN);
    if (length <= domain_length || contains_whitespace(identification)) {
        return false;
    }
    return equals_ignoring_case(identification + length - domain_length, IDENTIFICATION_DOMAIN);
}

/* Parses "cs230 HELLO <id>" with a valid id into identification; false otherwise. */
static bool parse_hello(const char *line, char *identification, size_t capacity) {
    size_t prefix_length = strlen(HELLO_PREFIX);
    const char *candidate = line + prefix_length;
    if (strncmp(line, HELLO_PREFIX, prefix_length) != 0 || strlen(candidate) >= capacity) {
        return false;
    }
    if (!is_valid_identification(candidate)) {
        return false;
    }
    strcpy(identification, candidate);
    return true;
}

/* Writes "cs230 STATUS <left> <op> <right>\n"; false when it does not fit. */
static bool format_status(const MathProblem *problem, char *line, size_t capacity) {
    int written = snprintf(line, capacity, "%s%lld %c %lld\n", STATUS_PREFIX,
                           problem->left, problem->operation, problem->right);
    return written > 0 && (size_t)written < capacity;
}

/* True when line is byte-exactly "cs230 <answer>" for the problem. */
static bool is_correct_answer(const MathProblem *problem, const char *line) {
    char expected[MAX_LINE_LENGTH];
    int written = snprintf(expected, sizeof expected, "%s%lld", PROTOCOL_PREFIX, evaluate(problem));
    return written > 0 && (size_t)written < sizeof expected && strcmp(expected, line) == 0;
}

/* Writes "cs230 <flag> BYE\n"; false when it does not fit. */
static bool format_bye(const char *flag, char *line, size_t capacity) {
    int written = snprintf(line, capacity, "%s%s%s\n", PROTOCOL_PREFIX, flag, BYE_SUFFIX);
    return written > 0 && (size_t)written < capacity;
}

/* ---- Configuration ------------------------------------------------------ */

/* Prints the command-line usage on stderr. */
static void print_usage(const char *program) {
    fprintf(stderr, "usage: %s [PORT]   (default: $%s, then %d)\n", program, PORT_VARIABLE, DEFAULT_PORT);
}

/* Parses an unsigned decimal in 0..maximum; rejects signs, whitespace and trailing text. */
static bool parse_bounded_decimal(const char *text, long maximum, long *value) {
    char *end;
    if (!isdigit((unsigned char)text[0])) {
        return false;
    }
    errno = 0;
    *value = strtol(text, &end, 10);
    return errno == 0 && *end == '\0' && *value <= maximum;
}

/* Parses a port in 0..65535; 0 asks the kernel for an ephemeral port. */
static bool parse_port(const char *text, unsigned short *port) {
    long value;
    if (!parse_bounded_decimal(text, MAX_PORT, &value)) {
        return false;
    }
    *port = (unsigned short)value;
    return true;
}

/* Parses the MATHBOT_PROBLEMS override in 1..MAX_PROBLEM_OVERRIDE. */
static bool parse_problem_override(const char *text, long *count) {
    return parse_bounded_decimal(text, MAX_PROBLEM_OVERRIDE, count) && *count >= 1;
}

/* Parses the MATHBOT_SEED override in 1..UINT32_MAX (0 would mean "unset"). */
static bool parse_seed_override(const char *text, uint32_t *seed) {
    long value;
    if (!parse_bounded_decimal(text, (long)UINT32_MAX, &value) || value < 1) {
        return false;
    }
    *seed = (uint32_t)value;
    return true;
}

/* The environment variable's value, or NULL when it is unset or empty. */
static const char *variable_or_null(const char *name) {
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : NULL;
}

/* Chooses the port: the argument, else MATHBOT_PORT, else DEFAULT_PORT; reports bad text. */
static bool load_port(int argc, char **argv, unsigned short *port) {
    const char *text = argc > 1 ? argv[1] : variable_or_null(PORT_VARIABLE);
    if (text == NULL) {
        *port = DEFAULT_PORT;
        return true;
    }
    if (!parse_port(text, port)) {
        fprintf(stderr, "invalid port '%s': expected 0..%d\n", text, MAX_PORT);
        return false;
    }
    return true;
}

/* Reads MATHBOT_PROBLEMS into the override, 0 when unset; reports bad text. */
static bool load_problem_override(long *override) {
    const char *text = variable_or_null(PROBLEMS_VARIABLE);
    if (text == NULL) {
        *override = 0;
        return true;
    }
    if (!parse_problem_override(text, override)) {
        fprintf(stderr, "invalid %s '%s': expected 1..%d\n", PROBLEMS_VARIABLE, text, MAX_PROBLEM_OVERRIDE);
        return false;
    }
    return true;
}

/* Reads MATHBOT_SEED into the override, 0 when unset; reports bad text. */
static bool load_seed_override(uint32_t *override) {
    const char *text = variable_or_null(SEED_VARIABLE);
    if (text == NULL) {
        *override = 0;
        return true;
    }
    if (!parse_seed_override(text, override)) {
        fprintf(stderr, "invalid %s '%s': expected 1..%lu\n", SEED_VARIABLE, text, (unsigned long)UINT32_MAX);
        return false;
    }
    return true;
}

/* Validates argv and the environment into config; false after reporting the reason. */
static bool load_config(int argc, char **argv, ServerConfig *config) {
    const char *secret;
    if (argc - 1 > MAX_ARGUMENTS) {
        fprintf(stderr, "expected at most %d argument, got %d\n", MAX_ARGUMENTS, argc - 1);
        return false;
    }
    if (!load_port(argc, argv, &config->port) || !load_problem_override(&config->problem_override) ||
        !load_seed_override(&config->seed_override)) {
        return false;
    }
    secret = getenv(SECRET_VARIABLE);
    config->secret = secret != NULL ? secret : "";
    return true;
}

/* ---- Signals -------------------------------------------------------------- */

/* SIGCHLD handler: reaps every finished child without blocking; keeps errno intact. */
static void reap_children(int signum) {
    int saved_errno = errno;
    (void)signum;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

/* SIGINT/SIGTERM handler: asks the accept loop to stop. */
static void request_shutdown(int signum) {
    (void)signum;
    shutdown_requested = 1;
}

/* Installs handler for signum with the given sigaction flags; reports failure. */
static bool install_handler(int signum, void (*handler)(int), int flags) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = handler;
    action.sa_flags = flags;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(signum, &action, NULL) != 0) {
        fprintf(stderr, "sigaction(%d): %s\n", signum, strerror(errno));
        return false;
    }
    return true;
}

/* Parent process signals: reap children, stop on INT/TERM, survive a peer's early close. */
static bool install_signal_handlers(void) {
    return install_handler(SIGCHLD, reap_children, SA_RESTART | SA_NOCLDSTOP) &&
           install_handler(SIGINT, request_shutdown, 0) &&
           install_handler(SIGTERM, request_shutdown, 0) &&
           install_handler(SIGPIPE, SIG_IGN, 0);
}

/* Child process signals: INT/TERM kill the session again; SIGPIPE stays ignored. */
static bool restore_default_signals(void) {
    return install_handler(SIGCHLD, SIG_DFL, 0) &&
           install_handler(SIGINT, SIG_DFL, 0) &&
           install_handler(SIGTERM, SIG_DFL, 0);
}

/* ---- Sockets ------------------------------------------------------------ */

/* Closes a socket, reporting (but not otherwise handling) a failure. */
static bool close_socket(int socket_fd) {
    if (close(socket_fd) != 0) {
        fprintf(stderr, "close: %s\n", strerror(errno));
        return false;
    }
    return true;
}

/* Sets SO_RCVTIMEO so recv (and accept, on Linux) gives up after timeout. */
static bool set_receive_timeout(int socket_fd, struct timeval timeout) {
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0) {
        fprintf(stderr, "setsockopt(SO_RCVTIMEO): %s\n", strerror(errno));
        return false;
    }
    return true;
}

/* Allows an immediate restart while old connections linger in TIME_WAIT. */
static bool allow_address_reuse(int socket_fd) {
    int enabled = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof enabled) != 0) {
        fprintf(stderr, "setsockopt(SO_REUSEADDR): %s\n", strerror(errno));
        return false;
    }
    return true;
}

/* Binds to 0.0.0.0:port so the server is reachable inside a container too. */
static bool bind_any_address(int socket_fd, unsigned short port) {
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof address) != 0) {
        fprintf(stderr, "bind to port %u: %s\n", (unsigned int)port, strerror(errno));
        return false;
    }
    return true;
}

/* Opens the listening socket; returns the descriptor or -1 after reporting. */
static int open_listener(unsigned short port) {
    struct timeval accept_poll = { ACCEPT_POLL_SECONDS, 0 };
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return -1;
    }
    if (!allow_address_reuse(listener) || !bind_any_address(listener, port) ||
        !set_receive_timeout(listener, accept_poll)) {
        (void)close_socket(listener);
        return -1;
    }
    if (listen(listener, LISTEN_BACKLOG) != 0) {
        fprintf(stderr, "listen: %s\n", strerror(errno));
        (void)close_socket(listener);
        return -1;
    }
    return listener;
}

/* The port the listener actually got, which matters when 0 was requested. */
static bool bound_port(int listener, unsigned short *port) {
    struct sockaddr_in address;
    socklen_t length = sizeof address;
    if (getsockname(listener, (struct sockaddr *)&address, &length) != 0) {
        fprintf(stderr, "getsockname: %s\n", strerror(errno));
        return false;
    }
    *port = ntohs(address.sin_port);
    return true;
}

/* Formats a peer as "a.b.c.d:port" for the log. */
static void format_peer(const struct sockaddr_in *address, char *text, size_t capacity) {
    char host[INET_ADDRSTRLEN] = "?";
    (void)inet_ntop(AF_INET, &address->sin_addr, host, sizeof host);
    (void)snprintf(text, capacity, "%s:%u", host, (unsigned int)ntohs(address->sin_port));
}

/* Writes one log line to stdout and flushes it so forked children never duplicate it. */
static void log_line(const char *message) {
    (void)printf("%s\n", message);
    (void)fflush(stdout);
}

/* Writes one per-session log line: "<peer> <message>". */
static void log_session(const char *peer, const char *message) {
    (void)printf("%s %s\n", peer, message);
    (void)fflush(stdout);
}

/* Appends received bytes to the buffer; false when they do not fit. */
static bool line_buffer_append(LineBuffer *buffer, const char *bytes, size_t count) {
    if (count > sizeof buffer->data - buffer->used) {
        return false;
    }
    memcpy(buffer->data + buffer->used, bytes, count);
    buffer->used += count;
    return true;
}

/* Copies the first complete line (without its newline) into line and removes it from
 * the buffer. A line that exceeds capacity or carries a NUL byte, which strcmp could
 * not see, is malformed. */
static TakeResult line_buffer_take_line(LineBuffer *buffer, char *line, size_t capacity) {
    const char *newline = memchr(buffer->data, '\n', buffer->used);
    size_t length;
    if (newline == NULL) {
        return TAKE_INCOMPLETE;
    }
    length = (size_t)(newline - buffer->data);
    if (length >= capacity || memchr(buffer->data, '\0', length) != NULL) {
        return TAKE_MALFORMED;
    }
    memcpy(line, buffer->data, length);
    line[length] = '\0';
    buffer->used -= length + 1;
    memmove(buffer->data, buffer->data + length + 1, buffer->used);
    return TAKE_LINE;
}

/* Classifies a failed recv: a timeout, or an error worth reporting. */
static ReceiveResult classify_receive_failure(void) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return RECEIVE_TIMEOUT;
    }
    fprintf(stderr, "recv: %s\n", strerror(errno));
    return RECEIVE_ERROR;
}

/* Reads from the socket until one complete line is available in line. */
static ReceiveResult receive_line(int socket_fd, LineBuffer *buffer, char *line, size_t capacity) {
    TakeResult taken;
    while ((taken = line_buffer_take_line(buffer, line, capacity)) == TAKE_INCOMPLETE) {
        char chunk[RECEIVE_BUFFER_SIZE];
        ssize_t received = recv(socket_fd, chunk, sizeof chunk, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            return classify_receive_failure();
        }
        if (received == 0) {
            return RECEIVE_EOF;
        }
        if (!line_buffer_append(buffer, chunk, (size_t)received)) {
            return RECEIVE_PROTOCOL_ERROR;
        }
    }
    return taken == TAKE_LINE ? RECEIVE_LINE : RECEIVE_PROTOCOL_ERROR;
}

/* Sends every byte, looping because a stream send may write fewer bytes than asked. */
static bool send_all(int socket_fd, const char *bytes, size_t count) {
    size_t sent_total = 0;
    while (sent_total < count) {
        ssize_t sent = send(socket_fd, bytes + sent_total, count - sent_total, 0);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0) {
            fprintf(stderr, "send: %s\n", strerror(errno));
            return false;
        }
        sent_total += (size_t)sent;
    }
    return true;
}

/* ---- One session (runs in the forked child) ----------------------------- */

/* Maps a receive failure onto the session outcome it ends with. */
static SessionOutcome outcome_of_receive(ReceiveResult result) {
    switch (result) {
    case RECEIVE_EOF:
        return SESSION_CLIENT_LEFT;
    case RECEIVE_TIMEOUT:
        return SESSION_TIMED_OUT;
    case RECEIVE_PROTOCOL_ERROR:
        return SESSION_MALFORMED_LINE;
    default:
        return SESSION_FAILED;
    }
}

/* The log text for an outcome. */
static const char *describe_outcome(SessionOutcome outcome) {
    switch (outcome) {
    case SESSION_FLAG_SENT:
        return "flag sent";
    case SESSION_BAD_HELLO:
        return "closed: bad HELLO";
    case SESSION_WRONG_ANSWER:
        return "closed: wrong answer";
    case SESSION_CLIENT_LEFT:
        return "closed: client hung up";
    case SESSION_TIMED_OUT:
        return "closed: timed out";
    case SESSION_MALFORMED_LINE:
        return "closed: malformed line";
    default:
        return "closed: socket error";
    }
}

/* The seed for this session: MATHBOT_SEED verbatim when set (every session then asks the
 * same problems, which is what a reproducible test wants), else the time mixed with the pid. */
static uint32_t session_seed(const ServerConfig *config) {
    if (config->seed_override != 0) {
        return config->seed_override;
    }
    return (uint32_t)time(NULL) * 2654435761u ^ (uint32_t)getpid();
}

/* The override from MATHBOT_PROBLEMS when set, else the spec's random count. */
static long session_problem_count(const ServerConfig *config, uint32_t *state) {
    return config->problem_override > 0 ? config->problem_override : problem_count(state);
}

/* Requires the first line to be a valid HELLO and keeps its identification. */
static SessionOutcome receive_hello(int connection, LineBuffer *buffer, char *identification, size_t capacity) {
    char line[RECEIVE_BUFFER_SIZE];
    ReceiveResult result = receive_line(connection, buffer, line, sizeof line);
    if (result != RECEIVE_LINE) {
        return outcome_of_receive(result);
    }
    return parse_hello(line, identification, capacity) ? SESSION_OK : SESSION_BAD_HELLO;
}

/* Sends one STATUS line and requires the byte-exact answer. */
static SessionOutcome ask_problem(int connection, LineBuffer *buffer, const MathProblem *problem) {
    char line[RECEIVE_BUFFER_SIZE];
    ReceiveResult result;
    if (!format_status(problem, line, sizeof line) || !send_all(connection, line, strlen(line))) {
        return SESSION_FAILED;
    }
    result = receive_line(connection, buffer, line, sizeof line);
    if (result != RECEIVE_LINE) {
        return outcome_of_receive(result);
    }
    return is_correct_answer(problem, line) ? SESSION_OK : SESSION_WRONG_ANSWER;
}

/* Asks count problems in turn, stopping at the first one not answered correctly. */
static SessionOutcome ask_problems(int connection, LineBuffer *buffer, uint32_t *state, long count) {
    long asked;
    for (asked = 0; asked < count; asked++) {
        MathProblem problem = make_problem(state);
        SessionOutcome outcome = ask_problem(connection, buffer, &problem);
        if (outcome != SESSION_OK) {
            return outcome;
        }
    }
    return SESSION_OK;
}

/* Sends "cs230 <flag> BYE\n" for the identification. */
static SessionOutcome send_bye(int connection, const char *secret, const char *identification) {
    char flag[FLAG_LENGTH + 1];
    char line[RECEIVE_BUFFER_SIZE];
    derive_flag(secret, identification, flag);
    if (!format_bye(flag, line, sizeof line) || !send_all(connection, line, strlen(line))) {
        return SESSION_FAILED;
    }
    return SESSION_FLAG_SENT;
}

/* One math-speak session from HELLO to BYE; returns why it ended. */
static SessionOutcome run_session(int connection, const ServerConfig *config) {
    LineBuffer buffer = { {0}, 0 };
    char identification[MAX_LINE_LENGTH];
    uint32_t state = session_seed(config);
    long count = session_problem_count(config, &state);
    SessionOutcome outcome = receive_hello(connection, &buffer, identification, sizeof identification);
    if (outcome == SESSION_OK) {
        outcome = ask_problems(connection, &buffer, &state, count);
    }
    if (outcome == SESSION_OK) {
        outcome = send_bye(connection, config->secret, identification);
    }
    return outcome;
}

/* Logs the connection, runs the session under the recv timeout, logs and returns the outcome. */
static SessionOutcome serve_connection(int connection, const ServerConfig *config, const char *peer) {
    struct timeval timeout = { SESSION_TIMEOUT_SECONDS, 0 };
    SessionOutcome outcome = SESSION_FAILED;
    log_session(peer, "connected");
    if (set_receive_timeout(connection, timeout)) {
        outcome = run_session(connection, config);
    }
    log_session(peer, describe_outcome(outcome));
    return outcome;
}

/* The child's whole life: drop the listener, serve, close, exit. The status is
 * EXIT_FAILURE only when the server itself failed; a misbehaving client is not our error. */
static void run_child(int listener, int connection, const ServerConfig *config, const char *peer) {
    SessionOutcome outcome;
    (void)close_socket(listener);
    (void)restore_default_signals();
    outcome = serve_connection(connection, config, peer);
    (void)close_socket(connection);
    exit(outcome == SESSION_FAILED ? EXIT_FAILURE : EXIT_SUCCESS);
}

/* ---- Accept loop (the parent) ------------------------------------------- */

/* Forks a child for the accepted connection; the parent keeps only the listener. */
static void spawn_session(int listener, int connection, const ServerConfig *config,
                          const struct sockaddr_in *peer_address) {
    char peer[PEER_TEXT_CAPACITY];
    pid_t child;
    format_peer(peer_address, peer, sizeof peer);
    child = fork();
    if (child < 0) {
        fprintf(stderr, "fork: %s\n", strerror(errno));
        log_session(peer, "rejected: fork failed");
    } else if (child == 0) {
        run_child(listener, connection, config, peer);
    }
    (void)close_socket(connection);
}

/* True for accept failures that mean "try again": the poll timeout, a signal, an aborted peer. */
static bool is_transient_accept_failure(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR || error == ECONNABORTED;
}

/* Accepts connections until SIGINT/SIGTERM; returns the exit status. */
static int accept_forever(int listener, const ServerConfig *config) {
    while (!shutdown_requested) {
        struct sockaddr_in peer_address;
        socklen_t length = sizeof peer_address;
        int connection = accept(listener, (struct sockaddr *)&peer_address, &length);
        if (connection >= 0) {
            spawn_session(listener, connection, config, &peer_address);
        } else if (!is_transient_accept_failure(errno)) {
            fprintf(stderr, "accept: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }
    }
    log_line("shutting down");
    return EXIT_SUCCESS;
}

/* Announces the bound port as the first stdout line, so a script can read it. */
static bool announce_listening(int listener) {
    unsigned short port;
    char message[MAX_LINE_LENGTH];
    if (!bound_port(listener, &port)) {
        return false;
    }
    (void)snprintf(message, sizeof message, "listening on port %u", (unsigned int)port);
    log_line(message);
    return true;
}

/* Entry point: configure, install handlers, listen, announce, accept until told to stop. */
int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "mathbot_server";
    ServerConfig config;
    int listener;
    int status;
    if (!load_config(argc, argv, &config)) {
        print_usage(program);
        return EXIT_FAILURE;
    }
    if (!install_signal_handlers()) {
        return EXIT_FAILURE;
    }
    listener = open_listener(config.port);
    if (listener < 0) {
        return EXIT_FAILURE;
    }
    status = announce_listening(listener) ? accept_forever(listener, &config) : EXIT_FAILURE;
    if (!close_socket(listener)) {
        status = EXIT_FAILURE;
    }
    return status;
}
