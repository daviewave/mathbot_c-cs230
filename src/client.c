/* client.c: CS230 Math Bot client. Connects to the math server, answers each
 * STATUS problem and prints the flag from the BYE message. Design notes live
 * in docs/design.md. */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

enum {
    RECEIVE_BUFFER_SIZE = 4096,
    EXPECTED_ARGUMENT_COUNT = 4,
    MIN_PORT = 1,
    MAX_PORT = 65535,
    MAX_IDENTIFICATION_LENGTH = 254,
    MESSAGE_CAPACITY = 512,
    FLAG_CAPACITY = 256
};

#define IDENTIFICATION_DOMAIN "@umass.edu"
#define PROTOCOL_PREFIX "cs230 "
#define HELLO_PREFIX "cs230 HELLO "
#define STATUS_PREFIX "cs230 STATUS "
#define BYE_SUFFIX " BYE"
#define VERBOSE_VARIABLE "MATHBOT_VERBOSE"

typedef struct {
    const char *identification;
    unsigned short port;
    const char *host;
} ClientArguments;

/* Bytes received from the server that have not been consumed as whole lines yet. */
typedef struct {
    char data[RECEIVE_BUFFER_SIZE];
    size_t used;
} LineBuffer;

/* One arithmetic problem from a STATUS line: left <operation> right. */
typedef struct {
    long long left;
    char operation;
    long long right;
} MathProblem;

typedef enum {
    RECEIVE_LINE,
    RECEIVE_EOF,
    RECEIVE_ERROR
} ReceiveResult;

enum {
    SESSION_CONTINUE,
    SESSION_DONE,
    SESSION_FAILED
};

/* Prints the command-line usage on stderr. */
static void print_usage(const char *program) {
    fprintf(stderr, "usage: %s <NetID@umass.edu> <port> <host IPv4 address>\n", program);
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

/* True for a non-empty NetID followed by @umass.edu, with no whitespace or second '@'. */
static bool is_valid_identification(const char *identification) {
    size_t length = strlen(identification);
    size_t domain_length = strlen(IDENTIFICATION_DOMAIN);
    const char *domain_start;
    if (length <= domain_length || length > MAX_IDENTIFICATION_LENGTH) {
        return false;
    }
    domain_start = identification + length - domain_length;
    if (strcmp(domain_start, IDENTIFICATION_DOMAIN) != 0 || strchr(identification, '@') != domain_start) {
        return false;
    }
    return !contains_whitespace(identification);
}

/* Parses a decimal port in 1..65535; rejects signs, whitespace and trailing text. */
static bool parse_port(const char *text, unsigned short *port) {
    char *end;
    long value;
    if (!isdigit((unsigned char)text[0])) {
        return false;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < MIN_PORT || value > MAX_PORT) {
        return false;
    }
    *port = (unsigned short)value;
    return true;
}

/* True when host is a dotted-quad IPv4 address. */
static bool is_valid_host(const char *host) {
    struct in_addr address;
    return inet_pton(AF_INET, host, &address) == 1;
}

/* Validates argv into arguments; on failure prints the reason on stderr and returns false. */
static bool parse_arguments(int argc, char **argv, ClientArguments *arguments) {
    if (argc != EXPECTED_ARGUMENT_COUNT) {
        fprintf(stderr, "expected 3 arguments, got %d\n", argc - 1);
        return false;
    }
    if (!is_valid_identification(argv[1])) {
        fprintf(stderr, "invalid identification '%s': expected NetID@umass.edu\n", argv[1]);
        return false;
    }
    if (!parse_port(argv[2], &arguments->port)) {
        fprintf(stderr, "invalid port '%s': expected 1..65535\n", argv[2]);
        return false;
    }
    if (!is_valid_host(argv[3])) {
        fprintf(stderr, "invalid host '%s': expected an IPv4 address\n", argv[3]);
        return false;
    }
    arguments->identification = argv[1];
    arguments->host = argv[3];
    return true;
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

/* Copies the first complete line (without its newline) into line and removes it
 * from the buffer; false when no complete line is buffered or it exceeds capacity. */
static bool line_buffer_take_line(LineBuffer *buffer, char *line, size_t capacity) {
    const char *newline = memchr(buffer->data, '\n', buffer->used);
    size_t length;
    size_t consumed;
    if (newline == NULL) {
        return false;
    }
    length = (size_t)(newline - buffer->data);
    if (length >= capacity) {
        return false;
    }
    memcpy(line, buffer->data, length);
    line[length] = '\0';
    consumed = length + 1;
    buffer->used -= consumed;
    memmove(buffer->data, buffer->data + consumed, buffer->used);
    return true;
}

/* Reads from the socket until one complete line is available in line. */
static ReceiveResult receive_line(int socket_fd, LineBuffer *buffer, char *line, size_t capacity) {
    while (!line_buffer_take_line(buffer, line, capacity)) {
        char chunk[RECEIVE_BUFFER_SIZE];
        ssize_t received = recv(socket_fd, chunk, sizeof chunk, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            fprintf(stderr, "recv: %s\n", strerror(errno));
            return RECEIVE_ERROR;
        }
        if (received == 0) {
            return RECEIVE_EOF;
        }
        if (!line_buffer_append(buffer, chunk, (size_t)received)) {
            fprintf(stderr, "protocol error: line longer than %d bytes\n", RECEIVE_BUFFER_SIZE);
            return RECEIVE_ERROR;
        }
    }
    return RECEIVE_LINE;
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

/* Sends one newline-terminated protocol line, echoing it on stderr when verbose. */
static bool send_line(int socket_fd, const char *line, bool verbose) {
    if (verbose) {
        fprintf(stderr, ">> %s", line);
    }
    return send_all(socket_fd, line, strlen(line));
}

/* Sends the identification message "cs230 HELLO <id>\n". */
static bool send_hello(int socket_fd, const char *identification, bool verbose) {
    char message[MESSAGE_CAPACITY];
    int written = snprintf(message, sizeof message, "%s%s\n", HELLO_PREFIX, identification);
    if (written < 0 || (size_t)written >= sizeof message) {
        fprintf(stderr, "identification too long to send\n");
        return false;
    }
    return send_line(socket_fd, message, verbose);
}

/* Closes a socket, reporting (but not otherwise handling) a failure. */
static bool close_socket(int socket_fd) {
    if (close(socket_fd) != 0) {
        fprintf(stderr, "close: %s\n", strerror(errno));
        return false;
    }
    return true;
}

/* Opens a TCP connection to host:port; returns the descriptor or -1. */
static int connect_to_server(const char *host, unsigned short port) {
    struct sockaddr_in address;
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return -1;
    }
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        fprintf(stderr, "invalid host '%s'\n", host);
        close_socket(socket_fd);
        return -1;
    }
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof address) != 0) {
        fprintf(stderr, "connect to %s:%u: %s\n", host, (unsigned int)port, strerror(errno));
        close_socket(socket_fd);
        return -1;
    }
    return socket_fd;
}

/* Parses a long long starting exactly at text (no leading whitespace or '+',
 * which strtoll would accept); end receives the first unparsed character. */
static bool parse_operand(const char *text, const char **end, long long *value) {
    char *parse_end;
    if (text[0] != '-' && !isdigit((unsigned char)text[0])) {
        return false;
    }
    errno = 0;
    *value = strtoll(text, &parse_end, 10);
    if (errno == ERANGE || parse_end == text) {
        return false;
    }
    *end = parse_end;
    return true;
}

/* True for the four operators math speak uses. */
static bool is_operator(char candidate) {
    return candidate == '+' || candidate == '-' || candidate == '*' || candidate == '/';
}

/* Parses "cs230 STATUS <num> <op> <num>" with single spaces and nothing else. */
static bool parse_status(const char *line, MathProblem *problem) {
    const char *cursor;
    if (strncmp(line, STATUS_PREFIX, strlen(STATUS_PREFIX)) != 0) {
        return false;
    }
    cursor = line + strlen(STATUS_PREFIX);
    if (!parse_operand(cursor, &cursor, &problem->left)) {
        return false;
    }
    if (cursor[0] != ' ' || !is_operator(cursor[1]) || cursor[2] != ' ') {
        return false;
    }
    problem->operation = cursor[1];
    cursor += 3;
    if (!parse_operand(cursor, &cursor, &problem->right)) {
        return false;
    }
    return *cursor == '\0';
}

/* Parses "cs230 <flag> BYE" into flag; the flag is one non-empty token without spaces. */
static bool parse_bye(const char *line, char *flag, size_t capacity) {
    size_t prefix_length = strlen(PROTOCOL_PREFIX);
    size_t suffix_length = strlen(BYE_SUFFIX);
    size_t length = strlen(line);
    size_t flag_length;
    if (length < prefix_length + 1 + suffix_length) {
        return false;
    }
    if (strncmp(line, PROTOCOL_PREFIX, prefix_length) != 0 ||
        strcmp(line + length - suffix_length, BYE_SUFFIX) != 0) {
        return false;
    }
    flag_length = length - prefix_length - suffix_length;
    if (flag_length >= capacity || memchr(line + prefix_length, ' ', flag_length) != NULL) {
        return false;
    }
    memcpy(flag, line + prefix_length, flag_length);
    flag[flag_length] = '\0';
    return true;
}

/* left + right without overflow; false when the sum does not fit. */
static bool add_checked(long long left, long long right, long long *result) {
    if ((right > 0 && left > LLONG_MAX - right) || (right < 0 && left < LLONG_MIN - right)) {
        return false;
    }
    *result = left + right;
    return true;
}

/* left - right without overflow; false when the difference does not fit. */
static bool subtract_checked(long long left, long long right, long long *result) {
    if ((right < 0 && left > LLONG_MAX + right) || (right > 0 && left < LLONG_MIN + right)) {
        return false;
    }
    *result = left - right;
    return true;
}

/* True when left * right fits, by sign case (CERT INT32-C adapted to long long). */
static bool multiplication_fits(long long left, long long right) {
    if (left > 0) {
        return right > 0 ? left <= LLONG_MAX / right : right >= LLONG_MIN / left;
    }
    if (right > 0) {
        return left >= LLONG_MIN / right;
    }
    return left == 0 || right >= LLONG_MAX / left;
}

/* left * right without overflow; false when the product does not fit. */
static bool multiply_checked(long long left, long long right, long long *result) {
    if (!multiplication_fits(left, right)) {
        return false;
    }
    *result = left * right;
    return true;
}

/* left / right truncated toward zero (C99 6.5.5); false for the two undefined cases. */
static bool divide_checked(long long left, long long right, long long *result) {
    if (right == 0 || (left == LLONG_MIN && right == -1)) {
        return false;
    }
    *result = left / right;
    return true;
}

/* Computes the problem; false on division by zero, overflow or an unknown operator. */
static bool evaluate(const MathProblem *problem, long long *result) {
    switch (problem->operation) {
    case '+':
        return add_checked(problem->left, problem->right, result);
    case '-':
        return subtract_checked(problem->left, problem->right, result);
    case '*':
        return multiply_checked(problem->left, problem->right, result);
    case '/':
        return divide_checked(problem->left, problem->right, result);
    default:
        return false;
    }
}

/* Reports the server closing the connection before the flag arrived. */
static void report_early_disconnect(void) {
    fprintf(stderr, "server closed the connection before sending the flag "
                    "(wrong answer or protocol error)\n");
}

/* Solves one STATUS problem and sends "cs230 <answer>\n". */
static bool handle_status(int socket_fd, const MathProblem *problem, bool verbose) {
    long long answer;
    char message[MESSAGE_CAPACITY];
    int written;
    if (!evaluate(problem, &answer)) {
        fprintf(stderr, "protocol error: cannot evaluate %lld %c %lld\n",
                problem->left, problem->operation, problem->right);
        return false;
    }
    written = snprintf(message, sizeof message, "%s%lld\n", PROTOCOL_PREFIX, answer);
    if (written < 0 || (size_t)written >= sizeof message) {
        fprintf(stderr, "answer too long to send\n");
        return false;
    }
    return send_line(socket_fd, message, verbose);
}

/* Writes the captured flag on stdout, the only thing the client ever prints there. */
static bool print_flag(const char *flag) {
    if (printf("%s\n", flag) < 0 || fflush(stdout) != 0) {
        fprintf(stderr, "stdout: %s\n", strerror(errno));
        return false;
    }
    return true;
}

/* Answers a STATUS line, prints the flag of a BYE line, or reports anything else. */
static int handle_line(int socket_fd, const char *line, bool verbose) {
    MathProblem problem;
    char flag[FLAG_CAPACITY];
    if (parse_status(line, &problem)) {
        return handle_status(socket_fd, &problem, verbose) ? SESSION_CONTINUE : SESSION_FAILED;
    }
    if (parse_bye(line, flag, sizeof flag)) {
        return print_flag(flag) ? SESSION_DONE : SESSION_FAILED;
    }
    fprintf(stderr, "protocol error: unexpected message '%s'\n", line);
    return SESSION_FAILED;
}

/* Identifies to the server and processes its lines until BYE; returns the exit status. */
static int run_session(int socket_fd, const char *identification, bool verbose) {
    LineBuffer buffer = { {0}, 0 };
    char line[RECEIVE_BUFFER_SIZE];
    int outcome = SESSION_CONTINUE;
    if (!send_hello(socket_fd, identification, verbose)) {
        return EXIT_FAILURE;
    }
    while (outcome == SESSION_CONTINUE) {
        ReceiveResult result = receive_line(socket_fd, &buffer, line, sizeof line);
        if (result == RECEIVE_EOF) {
            report_early_disconnect();
            return EXIT_FAILURE;
        }
        if (result == RECEIVE_ERROR) {
            return EXIT_FAILURE;
        }
        if (verbose) {
            fprintf(stderr, "<< %s\n", line);
        }
        outcome = handle_line(socket_fd, line, verbose);
    }
    return outcome == SESSION_DONE ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* True when MATHBOT_VERBOSE is set to anything other than empty or "0". */
static bool is_verbose_enabled(void) {
    const char *value = getenv(VERBOSE_VARIABLE);
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

/* Entry point: ignore SIGPIPE, validate arguments, connect, run the session, close. */
int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "client";
    ClientArguments arguments;
    int socket_fd;
    int status;
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf(stderr, "signal: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (!parse_arguments(argc, argv, &arguments)) {
        print_usage(program);
        return EXIT_FAILURE;
    }
    socket_fd = connect_to_server(arguments.host, arguments.port);
    if (socket_fd < 0) {
        return EXIT_FAILURE;
    }
    status = run_session(socket_fd, arguments.identification, is_verbose_enabled());
    if (!close_socket(socket_fd)) {
        return EXIT_FAILURE;
    }
    return status;
}
