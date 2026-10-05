/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Portable drawing-status serializer and literal checks, driven by the shared
 * plank-drawing-handoff-v1 fixtures (contract revision
 * plank-drawing-handoff-v1+r3).
 *
 * Build and test invocation, frozen by contract section 12.2:
 *   cmake -S . -B <build>            # no CMAKE_BUILD_TYPE, so NDEBUG is unset
 *   cmake --build <build> --parallel
 *   ctest --test-dir <build> --output-on-failure
 *
 * This file uses explicit failure checks, never bare assert: CMake Release,
 * MinSizeRel and RelWithDebInfo define NDEBUG, which compiles assert out. The
 * checks below therefore stay valid under any configuration.
 *
 * Registered above the Linux guard in CMakeLists.txt so it runs on macOS.
 */
#include "drawing_status.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PLANK_HANDOFF_FIXTURES
#error "PLANK_HANDOFF_FIXTURES must be defined by CMake"
#endif

#define MANIFEST_SHA256 \
    "dd1125d8257f11431671fe116335c4e3c798c157207bdd576f3c44e724e95954"
#define CONTRACT_REVISION "plank-drawing-handoff-v1+r3"

static int failures = 0;

static void check(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

static void check_text(const char *actual, const char *expected, const char *what) {
    if (actual == NULL || expected == NULL || strcmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL %s: expected \"%s\", got \"%s\"\n", what,
                expected ? expected : "(null)", actual ? actual : "(null)");
        ++failures;
    }
}

/* ---- SHA-256, so a drifted fixture or manifest fails a test ---------------- */

typedef struct {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    size_t pending;
} Sha256;

static uint32_t rotate_right(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

static void sha256_compress(Sha256 *digest, const uint8_t *block) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
        0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
        0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
        0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
        0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
        0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
        0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
        0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
        0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int index;
    for (index = 0; index < 16; ++index)
        w[index] = ((uint32_t)block[index * 4] << 24) |
                   ((uint32_t)block[index * 4 + 1] << 16) |
                   ((uint32_t)block[index * 4 + 2] << 8) |
                   (uint32_t)block[index * 4 + 3];
    for (index = 16; index < 64; ++index) {
        const uint32_t s0 = rotate_right(w[index - 15], 7) ^
                            rotate_right(w[index - 15], 18) ^ (w[index - 15] >> 3);
        const uint32_t s1 = rotate_right(w[index - 2], 17) ^
                            rotate_right(w[index - 2], 19) ^ (w[index - 2] >> 10);
        w[index] = w[index - 16] + s0 + w[index - 7] + s1;
    }
    a = digest->state[0]; b = digest->state[1]; c = digest->state[2];
    d = digest->state[3]; e = digest->state[4]; f = digest->state[5];
    g = digest->state[6]; h = digest->state[7];
    for (index = 0; index < 64; ++index) {
        const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + k[index] + w[index];
        const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    digest->state[0] += a; digest->state[1] += b; digest->state[2] += c;
    digest->state[3] += d; digest->state[4] += e; digest->state[5] += f;
    digest->state[6] += g; digest->state[7] += h;
}

static void sha256(const uint8_t *data, size_t length, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    Sha256 digest;
    uint8_t tail[128];
    size_t index, tail_length;
    uint64_t bits = (uint64_t)length * 8u;
    digest.state[0] = 0x6a09e667u; digest.state[1] = 0xbb67ae85u;
    digest.state[2] = 0x3c6ef372u; digest.state[3] = 0xa54ff53au;
    digest.state[4] = 0x510e527fu; digest.state[5] = 0x9b05688cu;
    digest.state[6] = 0x1f83d9abu; digest.state[7] = 0x5be0cd19u;
    digest.length = 0; digest.pending = 0;
    for (index = 0; index + 64 <= length; index += 64)
        sha256_compress(&digest, data + index);
    tail_length = length - index;
    memcpy(tail, data + index, tail_length);
    tail[tail_length++] = 0x80;
    while (tail_length % 64 != 56) tail[tail_length++] = 0;
    for (index = 0; index < 8; ++index)
        tail[tail_length++] = (uint8_t)(bits >> (56 - index * 8));
    for (index = 0; index < tail_length; index += 64)
        sha256_compress(&digest, tail + index);
    for (index = 0; index < 8; ++index) {
        out[index * 8 + 0] = hex[(digest.state[index] >> 28) & 0xf];
        out[index * 8 + 1] = hex[(digest.state[index] >> 24) & 0xf];
        out[index * 8 + 2] = hex[(digest.state[index] >> 20) & 0xf];
        out[index * 8 + 3] = hex[(digest.state[index] >> 16) & 0xf];
        out[index * 8 + 4] = hex[(digest.state[index] >> 12) & 0xf];
        out[index * 8 + 5] = hex[(digest.state[index] >> 8) & 0xf];
        out[index * 8 + 6] = hex[(digest.state[index] >> 4) & 0xf];
        out[index * 8 + 7] = hex[digest.state[index] & 0xf];
    }
    out[64] = '\0';
}

/* ---- fixture loading ------------------------------------------------------ */

static char *fixture_bytes = NULL;
static size_t fixture_length = 0;
static char *manifest_bytes = NULL;
static size_t manifest_length = 0;

static char *read_file(const char *relative, size_t *length) {
    char path[1024];
    FILE *handle;
    char *buffer;
    long size;
    snprintf(path, sizeof(path), "%s/%s", PLANK_HANDOFF_FIXTURES, relative);
    handle = fopen(path, "rb");
    if (handle == NULL) {
        fprintf(stderr, "FAIL cannot open fixture %s\n", path);
        ++failures;
        return NULL;
    }
    fseek(handle, 0, SEEK_END);
    size = ftell(handle);
    fseek(handle, 0, SEEK_SET);
    buffer = (char *)malloc((size_t)size + 1);
    if (buffer == NULL || size < 0 ||
        fread(buffer, 1, (size_t)size, handle) != (size_t)size) {
        fprintf(stderr, "FAIL cannot read fixture %s\n", path);
        ++failures;
        fclose(handle);
        free(buffer);
        return NULL;
    }
    fclose(handle);
    buffer[size] = '\0';
    *length = (size_t)size;
    return buffer;
}

/* Finds "<name>": "<value>" in the manifest entry for a fixture, or anywhere in
 * a small JSON text. Deliberately simple: these fixtures are stable text. */
static int find_string(const char *text, const char *member, char *out,
                       size_t capacity) {
    char needle[64];
    const char *cursor;
    size_t written = 0;
    snprintf(needle, sizeof(needle), "\"%s\"", member);
    cursor = strstr(text, needle);
    if (cursor == NULL) return 0;
    cursor += strlen(needle);
    while (*cursor == ' ' || *cursor == ':' || *cursor == '\n' || *cursor == '\t')
        ++cursor;
    if (*cursor != '"') return 0;
    ++cursor;
    while (*cursor != '\0' && *cursor != '"') {
        if (written + 1 >= capacity) return 0;
        out[written++] = *cursor++;
    }
    if (*cursor != '"') return 0;
    out[written] = '\0';
    return 1;
}

static int find_number(const char *text, const char *member, long *value) {
    char needle[64];
    const char *cursor;
    char *end;
    snprintf(needle, sizeof(needle), "\"%s\"", member);
    cursor = strstr(text, needle);
    if (cursor == NULL) return 0;
    cursor += strlen(needle);
    while (*cursor == ' ' || *cursor == ':' || *cursor == '\n' || *cursor == '\t')
        ++cursor;
    *value = strtol(cursor, &end, 10);
    return end != cursor;
}

/* The manifest entry for one fixture: the JSON object following its name. */
static const char *manifest_entry(const char *name) {
    char needle[256];
    const char *cursor;
    snprintf(needle, sizeof(needle), "\"%s\"", name);
    cursor = strstr(manifest_bytes, needle);
    if (cursor == NULL) return NULL;
    return cursor + strlen(needle);
}

/* Loads a fixture and verifies its SHA-256 against MANIFEST.json first. */
static int load_fixture(const char *name) {
    const char *entry = manifest_entry(name);
    char expected[128], digest[65];
    free(fixture_bytes);
    fixture_bytes = NULL;
    fixture_length = 0;
    if (entry == NULL) {
        fprintf(stderr, "FAIL manifest has no entry for %s\n", name);
        ++failures;
        return 0;
    }
    if (!find_string(entry, "sha256", expected, sizeof(expected))) {
        fprintf(stderr, "FAIL manifest entry for %s has no sha256\n", name);
        ++failures;
        return 0;
    }
    fixture_bytes = read_file(name, &fixture_length);
    if (fixture_bytes == NULL) return 0;
    sha256((const uint8_t *)fixture_bytes, fixture_length, digest);
    if (strcmp(digest, expected) != 0) {
        fprintf(stderr, "FAIL %s sha256 %s does not match manifest %s\n", name,
                digest, expected);
        ++failures;
        return 0;
    }
    return 1;
}

static const char *fixture_expectation(const char *name) {
    static char expectation[128];
    const char *entry = manifest_entry(name);
    if (entry == NULL) return NULL;
    if (!find_string(entry, "expect", expectation, sizeof(expectation))) return NULL;
    return expectation;
}

static int hex_to_key(const char *hex, uint8_t out[32]) {
    size_t index;
    if (strlen(hex) != 64) return 0;
    for (index = 0; index < 64; ++index) {
        const char character = hex[index];
        unsigned value;
        if (character >= '0' && character <= '9') value = (unsigned)(character - '0');
        else if (character >= 'a' && character <= 'f') value = (unsigned)(character - 'a') + 10u;
        else return 0;
        if (index % 2 == 0) out[index / 2] = (uint8_t)(value << 4);
        else out[index / 2] |= (uint8_t)value;
    }
    return 1;
}

/* ---- the checks ----------------------------------------------------------- */

static void manifest_pins_the_contract(void) {
    char digest[65];
    char revision[128];
    manifest_bytes = read_file("MANIFEST.json", &manifest_length);
    if (manifest_bytes == NULL) return;
    sha256((const uint8_t *)manifest_bytes, manifest_length, digest);
    check_text(digest, MANIFEST_SHA256, "MANIFEST.json SHA-256");
    if (find_string(manifest_bytes, "revision", revision, sizeof(revision)))
        check_text(revision, CONTRACT_REVISION, "manifest revision");
    else
        check(0, "manifest has a revision member");
}

static void valid_local_metadata_round_trips(void) {
    static const char *const ready[] = {"local/valid/wildcard.json",
                                        "local/valid/literal.json",
                                        "local/valid/loopback.json",
                                        "local/valid/link-local.json"};
    size_t index;
    for (index = 0; index < sizeof(ready) / sizeof(ready[0]); ++index) {
        char identity[128], address[64], expected[PLTR_DRAWING_STATUS_RESPONSE_MAX];
        char produced[PLTR_DRAWING_STATUS_RESPONSE_MAX];
        uint8_t key[32];
        long port = 0;
        int written;
        if (!load_fixture(ready[index])) continue;
        check_text(fixture_expectation(ready[index]), "accept", ready[index]);
        check(find_string(fixture_bytes, "drawingIdentity", identity, sizeof(identity)),
              "fixture carries drawingIdentity");
        check(find_string(fixture_bytes, "boundAddress", address, sizeof(address)),
              "fixture carries boundAddress");
        check(find_number(fixture_bytes, "boundPort", &port), "fixture carries boundPort");
        if (!hex_to_key(identity, key)) {
            check(0, "fixture drawingIdentity is 64 lowercase hex characters");
            continue;
        }
        check(pltr_drawing_status_check_bound_address(address) == PLTR_BOUND_ADDRESS_OK,
              "a valid fixture boundAddress is accepted");
        written = pltr_drawing_status_ready(key, address, (uint16_t)port, produced,
                                            sizeof(produced));
        check(written > 0, "ready metadata is serialized");
        if (written <= 0) continue;
        /* Compare semantic content, built from the fixture's own values. */
        snprintf(expected, sizeof(expected),
                 "{\"version\":1,\"ok\":true,\"supported\":true,\"state\":\"ready\","
                 "\"listener\":{\"drawingIdentity\":\"%s\","
                 "\"drawingProtocol\":{\"name\":\"pltr-raw-hid\",\"version\":1,"
                 "\"rawHID\":1,\"linkType\":2},"
                 "\"boundAddress\":\"%s\",\"boundPort\":%ld}}\n",
                 identity, address, port);
        check_text(produced, expected, "serialized ready metadata");
        check((size_t)written <= PLTR_DRAWING_STATUS_RESPONSE_MAX,
              "ready metadata is within the response bound");
        check(produced[written - 1] == '\n' &&
              strchr(produced, '\n') == produced + written - 1,
              "ready metadata ends with exactly one newline");
    }
}

static void valid_unavailable_metadata_round_trips(void) {
    char reason[128], expected[PLTR_DRAWING_STATUS_RESPONSE_MAX];
    char produced[PLTR_DRAWING_STATUS_RESPONSE_MAX];
    int written;
    if (!load_fixture("local/valid/unavailable.json")) return;
    check_text(fixture_expectation("local/valid/unavailable.json"), "accept",
               "local/valid/unavailable.json");
    check(find_string(fixture_bytes, "reason", reason, sizeof(reason)),
          "fixture carries a reason");
    written = pltr_drawing_status_unavailable(reason, produced, sizeof(produced));
    check(written > 0, "unavailable metadata is serialized");
    if (written <= 0) return;
    snprintf(expected, sizeof(expected),
             "{\"version\":1,\"ok\":true,\"supported\":true,"
             "\"state\":\"unavailable\",\"reason\":\"%s\"}\n", reason);
    check_text(produced, expected, "serialized unavailable metadata");
}

static void invalid_bound_addresses_report_their_reason(void) {
    static const char *const cases[] = {
        "local/invalid/bound-address-broadcast.json",
        "local/invalid/bound-address-hostname.json",
        "local/invalid/bound-address-ipv6.json",
        "local/invalid/bound-address-multicast.json",
        "local/invalid/bound-address-non-canonical.json",
        "local/invalid/bound-address-too-long.json",
        "local/invalid/bound-address-zero-net.json"};
    size_t index;
    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char address[64];
        const char *expectation;
        if (!load_fixture(cases[index])) continue;
        expectation = fixture_expectation(cases[index]);
        if (!find_string(fixture_bytes, "boundAddress", address, sizeof(address))) {
            check(0, "invalid fixture carries boundAddress");
            continue;
        }
        check_text(pltr_drawing_status_bound_address_reason(
                       pltr_drawing_status_check_bound_address(address)),
                   expectation, cases[index]);
        /* The serializer refuses to emit a listener it cannot describe. */
        {
            uint8_t key[32];
            char produced[PLTR_DRAWING_STATUS_RESPONSE_MAX];
            memset(key, 0x11, sizeof(key));
            check(pltr_drawing_status_ready(key, address, 28990, produced,
                                            sizeof(produced)) < 0,
                  "an unusable boundAddress is never serialized");
        }
    }
    if (load_fixture("local/invalid/bound-address-missing.json")) {
        char address[64];
        check(!find_string(fixture_bytes, "boundAddress", address, sizeof(address)),
              "the missing fixture has no boundAddress");
        check_text(pltr_drawing_status_bound_address_reason(
                       pltr_drawing_status_check_bound_address(NULL)),
                   fixture_expectation("local/invalid/bound-address-missing.json"),
                   "local/invalid/bound-address-missing.json");
    }
}

static void mapped_and_scoped_literals_are_named(void) {
    /* Section 7.4a step 4: the mapped test precedes the round-trip, so the
     * reason does not depend on how a platform renders the address. The texts
     * here are kept within 15 bytes because section 7.5 step 3 reports tooLong
     * before any literal pre-check; the longer section 7.4a examples are
     * checked for that ordering below. */
    check_text(pltr_drawing_status_bound_address_reason(
                   pltr_drawing_status_check_bound_address("::ffff:c000:20a")),
               "metadata.boundAddress.mapped", "IPv4-mapped IPv6");
    check_text(pltr_drawing_status_bound_address_reason(
                   pltr_drawing_status_check_bound_address("::c000:20a")),
               "metadata.boundAddress.mapped", "IPv4-compatible IPv6");
    check_text(pltr_drawing_status_bound_address_reason(
                   pltr_drawing_status_check_bound_address("2001:DB8::2")),
               "metadata.boundAddress.nonCanonical", "uppercase IPv6");
    check_text(pltr_drawing_status_bound_address_reason(
                   pltr_drawing_status_check_bound_address("2001:db8::0:2")),
               "metadata.boundAddress.nonCanonical", "non-minimal IPv6");
    check_text(pltr_drawing_status_bound_address_reason(
                   pltr_drawing_status_check_bound_address("192.0.2.1:1")),
               "metadata.boundAddress.notLiteral", "address with a port");
    /* Section 7.5 orders tooLong (step 3) before the literal pre-checks (step
     * 4), so a 17-byte mapped literal reports the length, not the family. */
    check_text(pltr_drawing_status_bound_address_reason(
                   pltr_drawing_status_check_bound_address("::ffff:192.0.2.10")),
               "metadata.boundAddress.tooLong", "17-byte mapped literal");
    check_text(pltr_drawing_status_bound_address_reason(
                   pltr_drawing_status_check_bound_address("::1")),
               "metadata.boundAddress.familyUnsupported", "IPv6 loopback");
    check_text(pltr_drawing_status_bound_address_reason(
                   pltr_drawing_status_check_bound_address("")),
               "metadata.boundAddress.notLiteral", "empty address");
    check_text(pltr_drawing_status_bound_address_reason(
                   pltr_drawing_status_check_bound_address("192.0.2")),
               "metadata.boundAddress.notLiteral", "three groups");
    check_text(pltr_drawing_status_bound_address_reason(
                   pltr_drawing_status_check_bound_address("192.0.2.256")),
               "metadata.boundAddress.notLiteral", "octet above 255");
    check(pltr_drawing_status_check_bound_address("0.0.0.0") == PLTR_BOUND_ADDRESS_OK,
          "the unspecified address is a legal bind");
    check(pltr_drawing_status_check_bound_address("127.0.0.1") == PLTR_BOUND_ADDRESS_OK,
          "loopback is a legal bind");
    check(pltr_drawing_status_check_bound_address("169.254.10.20") == PLTR_BOUND_ADDRESS_OK,
          "a link-local literal is a legal bind");
    check(pltr_drawing_status_check_bound_address("240.0.0.1") == PLTR_BOUND_ADDRESS_INVALID,
          "240/4 is not a usable bind");
}

static void requests_are_exactly_the_one_operation(void) {
    static const char *const accepted[] = {
        "{\"op\":\"drawing-status\",\"version\":1}\n",
        "{\"version\":1,\"op\":\"drawing-status\"}\n",
        "{\"op\":\"drawing-status\",\"version\":1}",
        "{ \"op\" : \"drawing-status\" , \"version\" : 1 }\n"};
    static const char *const refused[] = {
        "{\"op\":\"drawing-status\"}\n",
        "{\"version\":1}\n",
        "{\"op\":\"drawing-status\",\"version\":2}\n",
        "{\"op\":\"drawing-status\",\"version\":1,\"cached\":true}\n",
        "{\"op\":\"drawing-status\",\"op\":\"drawing-status\",\"version\":1}\n",
        "{\"op\":\"capture\",\"version\":1}\n",
        "{\"op\":\"drawing-status\",\"version\":1}{\n",
        "{\"op\":{\"nested\":1},\"version\":1}\n",
        "[\"drawing-status\"]\n",
        "{}\n",
        "{\"op\":\"drawing-status\",\"version\":10}\n",
        "{\"op\":\"drawing-status\",\"version\":1.0}\n"};
    char oversize[PLTR_DRAWING_STATUS_REQUEST_MAX + 64];
    size_t index;
    for (index = 0; index < sizeof(accepted) / sizeof(accepted[0]); ++index)
        check(pltr_drawing_status_check_request(accepted[index],
                                               strlen(accepted[index])) == 1,
              accepted[index]);
    for (index = 0; index < sizeof(refused) / sizeof(refused[0]); ++index)
        check(pltr_drawing_status_check_request(refused[index],
                                               strlen(refused[index])) == 0,
              refused[index]);
    memset(oversize, 'a', sizeof(oversize));
    oversize[sizeof(oversize) - 1] = '\0';
    check(pltr_drawing_status_check_request(oversize, strlen(oversize)) == 0,
          "a request over the 256-byte bound is refused");
}

static void peer_policy_is_uid_zero_in_production(void) {
    check(PLTR_DRAWING_STATUS_ACCEPTED_UID == 0u,
          "the production accepted peer uid is 0");
    check(pltr_drawing_status_peer_accepted(0u, PLTR_DRAWING_STATUS_ACCEPTED_UID) == 1,
          "uid 0 is accepted");
    check(pltr_drawing_status_peer_accepted(1000u, PLTR_DRAWING_STATUS_ACCEPTED_UID) == 0,
          "an ordinary user is refused");
    check(pltr_drawing_status_peer_accepted(65534u, PLTR_DRAWING_STATUS_ACCEPTED_UID) == 0,
          "nobody is refused");
    /* There is no "or the server's own uid" allowance: a non-zero uid equal to
     * the server's own account is still refused in production. */
    check(pltr_drawing_status_peer_accepted(998u, PLTR_DRAWING_STATUS_ACCEPTED_UID) == 0,
          "the raw service account is refused in production");
}

static void the_status_name_is_not_the_capture_name(void) {
    check_text(PLTR_DRAWING_STATUS_NAME, "plank-tablet-drawing-status-v1",
               "production status name");
    check(strcmp(PLTR_DRAWING_STATUS_NAME, "plank-tablet-capture-v1") != 0,
          "the status name is not the capture interlock name");
    check(PLTR_DRAWING_STATUS_REQUEST_MAX == 256, "request bound");
    check(PLTR_DRAWING_STATUS_RESPONSE_MAX == 4096, "response bound");
    check(PLTR_DRAWING_STATUS_CONNECTION_MS == 500, "server connection budget");
    check(PLTR_DRAWING_STATUS_BACKLOG == 4, "listen backlog");
}

static void malformed_reasons_are_never_serialized(void) {
    char produced[PLTR_DRAWING_STATUS_RESPONSE_MAX];
    check(pltr_drawing_status_unavailable("service absent", produced,
                                          sizeof(produced)) < 0,
          "a reason with a space is refused");
    check(pltr_drawing_status_unavailable("service.\"absent", produced,
                                          sizeof(produced)) < 0,
          "a reason with a quote is refused");
    check(pltr_drawing_status_unavailable("", produced, sizeof(produced)) < 0,
          "an empty reason is refused");
    check(pltr_drawing_status_unavailable("service.absent", produced, 16) < 0,
          "a short buffer is refused rather than truncated");
    check(pltr_drawing_status_error("Invalid drawing status request.", produced,
                                     sizeof(produced)) > 0,
          "the error reply is serialized");
    check_text(produced,
               "{\"version\":1,\"ok\":false,\"error\":"
               "\"Invalid drawing status request.\"}\n",
               "serialized error reply");
}

int main(void) {
    manifest_pins_the_contract();
    the_status_name_is_not_the_capture_name();
    peer_policy_is_uid_zero_in_production();
    valid_local_metadata_round_trips();
    valid_unavailable_metadata_round_trips();
    invalid_bound_addresses_report_their_reason();
    mapped_and_scoped_literals_are_named();
    requests_are_exactly_the_one_operation();
    const char *v2 = "{\"op\":\"drawing-status\",\"version\":2}\n";
    check(pltr_drawing_status_request_version(v2, strlen(v2)) == 2, "V2 request is distinct");
    check(!pltr_drawing_status_check_request(v2, strlen(v2)), "V1 validator remains frozen");
    const char *bad_v2 = "{\"op\":\"drawing-status\",\"version\":2,\"version\":2}\n";
    check(!pltr_drawing_status_request_version(bad_v2, strlen(bad_v2)), "V2 duplicate refused");
    malformed_reasons_are_never_serialized();
    free(fixture_bytes);
    free(manifest_bytes);
    if (failures != 0) {
        fprintf(stderr, "%d drawing-status check(s) failed\n", failures);
        return 1;
    }
    printf("drawing_status_test: all checks passed\n");
    return 0;
}
