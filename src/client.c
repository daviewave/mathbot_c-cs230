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
    EXPECTED_ARGUMENT_COUNT = 4,
    MIN_PORT = 1,
    MAX_PORT = 65535,
    MAX_IDENTIFICATION_LENGTH = 254
};

#define IDENTIFICATION_DOMAIN "@umass.edu"

typedef struct {
    const char *identification;
    unsigned short port;
    const char *host;
} ClientArguments;

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

/* Entry point: validates the arguments; the session is not implemented yet. */
int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "client";
    ClientArguments arguments;
    if (!parse_arguments(argc, argv, &arguments)) {
        print_usage(program);
        return EXIT_FAILURE;
    }
    return EXIT_FAILURE;
}
