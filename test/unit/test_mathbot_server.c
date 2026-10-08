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

int main(void) {
    test_sha256_standard_vectors();
    test_sha256_padding_boundaries();
    test_sha256_incremental_updates();
    test_hex_encode();
    test_derive_flag();
    CHECK_REPORT("test_mathbot_server");
}
