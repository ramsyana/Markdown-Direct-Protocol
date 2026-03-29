#include "mdp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "ASSERTION FAILED: %s at %s:%d\n", #expr, __FILE__, __LINE__); \
            exit(1); \
        } \
    } while (0)

static void test_build_and_parse_single_frame(void) {
    const char* payload = "{\"model\":\"x\",\"prompt\":\"hi\"}";

    uint8_t* frame = NULL;
    uint32_t frame_len = 0;
    EXPECT(mdp_build_frame(MDP_MSG_REQUEST, (const uint8_t*)payload, (uint32_t)strlen(payload), &frame, &frame_len));
    EXPECT(frame_len == 5u + strlen(payload));
    EXPECT(frame[0] == MDP_MSG_REQUEST);

    MdpParser p;
    mdp_parser_init(&p, 1024 * 1024);
    EXPECT(mdp_parser_feed(&p, frame, frame_len));

    MdpMessage m;
    EXPECT(mdp_parser_next(&p, &m));
    EXPECT(m.type == MDP_MSG_REQUEST);
    EXPECT(m.length == strlen(payload));
    EXPECT(memcmp(m.payload, payload, m.length) == 0);
    mdp_message_free(&m);

    EXPECT(!mdp_parser_next(&p, &m));

    mdp_parser_free(&p);
    free(frame);
}

static void test_incremental_feed_header_split(void) {
    const uint8_t payload[] = {0x41, 0x42, 0x43};

    uint8_t* frame = NULL;
    uint32_t frame_len = 0;
    EXPECT(mdp_build_frame(MDP_MSG_MARKDOWN_CHUNK, payload, (uint32_t)sizeof(payload), &frame, &frame_len));

    MdpParser p;
    mdp_parser_init(&p, 1024 * 1024);

    EXPECT(mdp_parser_feed(&p, frame, 2));
    MdpMessage m;
    EXPECT(!mdp_parser_next(&p, &m));

    EXPECT(mdp_parser_feed(&p, frame + 2, frame_len - 2));
    EXPECT(mdp_parser_next(&p, &m));
    EXPECT(m.type == MDP_MSG_MARKDOWN_CHUNK);
    EXPECT(m.length == sizeof(payload));
    EXPECT(memcmp(m.payload, payload, sizeof(payload)) == 0);
    mdp_message_free(&m);

    mdp_parser_free(&p);
    free(frame);
}

static void test_multiple_frames_back_to_back(void) {
    const char* p1 = "one";
    const char* p2 = "two-two";

    uint8_t *f1 = NULL, *f2 = NULL;
    uint32_t l1 = 0, l2 = 0;

    EXPECT(mdp_build_frame(0x99, (const uint8_t*)p1, (uint32_t)strlen(p1), &f1, &l1));
    EXPECT(mdp_build_frame(MDP_MSG_END_OF_RESPONSE, (const uint8_t*)p2, (uint32_t)strlen(p2), &f2, &l2));

    uint8_t* both = (uint8_t*)malloc((size_t)l1 + (size_t)l2);
    EXPECT(both);
    memcpy(both, f1, l1);
    memcpy(both + l1, f2, l2);

    MdpParser p;
    mdp_parser_init(&p, 1024 * 1024);
    EXPECT(mdp_parser_feed(&p, both, (size_t)l1 + (size_t)l2));

    MdpMessage m;
    EXPECT(mdp_parser_next(&p, &m));
    /* 0x99 is an unknown/future type — parser must pass it through unchanged */
    EXPECT(m.type == 0x99);
    EXPECT(m.length == strlen(p1));
    EXPECT(memcmp(m.payload, p1, m.length) == 0);
    mdp_message_free(&m);

    EXPECT(mdp_parser_next(&p, &m));
    EXPECT(m.type == MDP_MSG_END_OF_RESPONSE);
    EXPECT(m.length == strlen(p2));
    EXPECT(memcmp(m.payload, p2, m.length) == 0);
    mdp_message_free(&m);

    EXPECT(!mdp_parser_next(&p, &m));

    mdp_parser_free(&p);
    free(both);
    free(f1);
    free(f2);
}

static void test_utf8_safe_prefix(void) {
    const uint8_t s[] = "a" "\xE2\x82\xAC" "b"; /* a € b */
    size_t len = strlen((const char*)s);

    EXPECT(mdp_utf8_safe_prefix(s, len, 1) == 1);

    EXPECT(mdp_utf8_safe_prefix(s, len, 2) == 1);

    EXPECT(mdp_utf8_safe_prefix(s, len, 3) == 1);

    EXPECT(mdp_utf8_safe_prefix(s, len, 4) == 4);

    EXPECT(mdp_utf8_safe_prefix(s, len, 5) == 5);
}

static void test_parser_max_frame_size_protection(void) {
    MdpParser p;
    mdp_parser_init(&p, 100); // Very small limit

    /* 1. Feed 101 bytes -> should NOT fail in feed anymore (buffer growth is allowed) */
    uint8_t large[101];
    memset(large, 0, sizeof(large));
    EXPECT(mdp_parser_feed(&p, large, sizeof(large)) == true);
    EXPECT(p.has_error == false);

    mdp_parser_free(&p);
    mdp_parser_init(&p, 100);

    /* 2. Feed a frame that claims to be 1GB but we only have 100 byte limit */
    uint8_t malicious_header[] = {0x01, 0x3F, 0xFF, 0xFF, 0xFF}; // Metadata, length = 1,073,741,823
    EXPECT(mdp_parser_feed(&p, malicious_header, sizeof(malicious_header)));
    
    MdpMessage m;
    EXPECT(mdp_parser_next(&p, &m) == false);
    EXPECT(p.has_error == true);

    mdp_parser_free(&p);
}

static void test_mdp_build_frame_overflow(void) {
    uint8_t* b = NULL;
    uint32_t l = 0;
    /* Try to build a frame with a massive payload length that would overflow 32-bit uint */
    EXPECT(!mdp_build_frame(0x01, NULL, 0xFFFFFFFFu, &b, &l));
    EXPECT(!mdp_build_frame(0x01, NULL, 0xFFFFFFF1u, &b, &l));
}

static void test_mdp_validate_request_json(void) {
    /* Valid */
    const char* v1 = "{\"model\":\"m\",\"prompt\":\"p\"}";
    EXPECT(mdp_validate_request_json((const uint8_t*)v1, (uint32_t)strlen(v1)));

    /* Missing braces */
    const char* v2 = "\"model\":\"m\",\"prompt\":\"p\"";
    EXPECT(!mdp_validate_request_json((const uint8_t*)v2, (uint32_t)strlen(v2)));

    /* Missing keys */
    const char* v3 = "{\"foo\":\"bar\"}";
    EXPECT(!mdp_validate_request_json((const uint8_t*)v3, (uint32_t)strlen(v3)));

    /* Empty */
    EXPECT(!mdp_validate_request_json((const uint8_t*)"", 0));
}

static void test_utf8_prefix_edge_cases(void) {
    /* Invalid lead byte AT THE END remains 0 (truncated) */
    const uint8_t s1[] = {0xFF};
    EXPECT(mdp_utf8_safe_prefix(s1, 1, 1) == 0);

    /* Lead byte without completion at the end */
    const uint8_t s2[] = {0xC2}; // Expects 2 bytes
    EXPECT(mdp_utf8_safe_prefix(s2, 1, 1) == 0);
    
    /* Valid 2-byte char */
    const uint8_t s3[] = {0xC2, 0xA2}; // Cent sign
    EXPECT(mdp_utf8_safe_prefix(s3, 2, 2) == 2);
    EXPECT(mdp_utf8_safe_prefix(s3, 2, 1) == 0);
}

int main(void) {
    test_build_and_parse_single_frame();
    test_incremental_feed_header_split();
    test_multiple_frames_back_to_back();
    test_utf8_safe_prefix();
    test_parser_max_frame_size_protection();
    test_mdp_build_frame_overflow();
    test_mdp_validate_request_json();
    test_utf8_prefix_edge_cases();

    fprintf(stderr, "test_mdp: OK\n");
    return 0;
}

