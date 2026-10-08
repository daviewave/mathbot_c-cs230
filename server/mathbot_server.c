/* mathbot_server.c: a local stand-in for the CS230 math-speak server. Accepts
 * connections forever, forks one child per session, sends 300..2000 arithmetic
 * problems and ends a fully correct session with a SHA-256 flag. Design notes
 * live in docs/server.md. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SHA256_BLOCK_LENGTH = 64,
    SHA256_DIGEST_LENGTH = 32,
    SHA256_HEX_LENGTH = 64,
    SHA256_ROUNDS = 64,
    SHA256_STATE_WORDS = 8,
    SHA256_SCHEDULE_WORDS = 64,
    SHA256_LENGTH_FIELD = 8,
    FLAG_LENGTH = SHA256_HEX_LENGTH
};

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

/* One-shot SHA-256 of count bytes into digest (SHA256_DIGEST_LENGTH bytes). */
static void sha256_digest(const unsigned char *data, size_t count, unsigned char *digest) {
    Sha256 hash;
    sha256_init(&hash);
    sha256_update(&hash, data, count);
    sha256_final(&hash, digest);
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
