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

int main(void) {
    test_identification();
    test_parse_port();
    test_host();
    test_parse_arguments();
    CHECK_REPORT("test_client");
}
