/*
 * bench_mdp.c — MDP framing microbenchmark
 *
 * Measures MDP frame build/parse throughput and compares per-frame overhead
 * against a simulated HTTP/SSE envelope (data: ... \n\n + JSON wrapper).
 *
 * Build:  cmake --build build --target bench_mdp
 * Run:    ./build/bench_mdp
 *
 * No MsQuic dependency — benchmarks the framing layer only.
 */

#include "mdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define ITERATIONS  1000000
#define NUM_SIZES   3

static const size_t PAYLOAD_SIZES[NUM_SIZES] = {32, 512, 4096};
static const char*  SIZE_LABELS[NUM_SIZES]   = {"32 B", "512 B", "4 KB"};

/* ---------- helpers ---------- */

static double elapsed_ns(struct timespec start, struct timespec end) {
    return (double)(end.tv_sec - start.tv_sec) * 1e9 +
           (double)(end.tv_nsec - start.tv_nsec);
}

static void fill_payload(uint8_t* buf, size_t len) {
    /* Realistic-ish: mixed ASCII markdown content */
    const char* pattern = "# Hello World\n\n**Bold text** and `inline code` with some *italic* words.\n\n";
    size_t plen = strlen(pattern);
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)pattern[i % plen];
    }
}

/* ---------- MDP benchmark ---------- */

typedef struct BenchResult {
    double total_ns;
    double frames_per_sec;
    double mb_per_sec;
    double avg_ns_per_frame;
    size_t total_bytes;
} BenchResult;

static BenchResult bench_mdp_roundtrip(const uint8_t* payload, size_t payload_len, int iterations) {
    BenchResult r;
    memset(&r, 0, sizeof(r));

    struct timespec start, end;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);

    MdpParser parser;
    mdp_parser_init(&parser, 16 * 1024 * 1024);

    for (int i = 0; i < iterations; i++) {
        /* Build frame */
        uint8_t* frame = NULL;
        uint32_t frame_len = 0;
        if (!mdp_build_frame(MDP_MSG_MARKDOWN_CHUNK, payload, (uint32_t)payload_len, &frame, &frame_len)) {
            fprintf(stderr, "mdp_build_frame failed\n");
            exit(1);
        }

        r.total_bytes += frame_len;

        /* Feed into parser */
        if (!mdp_parser_feed(&parser, frame, frame_len)) {
            fprintf(stderr, "mdp_parser_feed failed\n");
            exit(1);
        }

        /* Parse out the message */
        MdpMessage msg;
        if (!mdp_parser_next(&parser, &msg)) {
            fprintf(stderr, "mdp_parser_next failed\n");
            exit(1);
        }
        mdp_message_free(&msg);
        free(frame);
    }

    mdp_parser_free(&parser);

    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
    r.total_ns = elapsed_ns(start, end);
    r.frames_per_sec = (double)iterations / (r.total_ns / 1e9);
    r.mb_per_sec = (double)r.total_bytes / (1024.0 * 1024.0) / (r.total_ns / 1e9);
    r.avg_ns_per_frame = r.total_ns / (double)iterations;
    return r;
}

/* ---------- HTTP/SSE simulation ---------- */

/*
 * Simulates the overhead of wrapping the same payload in an SSE envelope:
 *   "data: " + base64-ish wrapping + "\n\n"
 * Plus a minimal JSON container: {"type":"chunk","data":"..."}
 *
 * We do actual memcpy work to simulate real serialization cost.
 */

static const char SSE_PREFIX[] = "data: {\"type\":\"chunk\",\"data\":\"";
static const char SSE_SUFFIX[] = "\"}\n\n";

static BenchResult bench_http_sse_roundtrip(const uint8_t* payload, size_t payload_len, int iterations) {
    BenchResult r;
    memset(&r, 0, sizeof(r));

    size_t prefix_len = sizeof(SSE_PREFIX) - 1;
    size_t suffix_len = sizeof(SSE_SUFFIX) - 1;
    size_t envelope_len = prefix_len + payload_len + suffix_len;

    uint8_t* envelope = (uint8_t*)malloc(envelope_len);
    if (!envelope) {
        fprintf(stderr, "malloc failed\n");
        exit(1);
    }

    /* Pre-build the parse-side buffer */
    uint8_t* parse_buf = (uint8_t*)malloc(payload_len);
    if (!parse_buf) {
        fprintf(stderr, "malloc failed\n");
        exit(1);
    }

    struct timespec start, end;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);

    for (int i = 0; i < iterations; i++) {
        /* --- Serialize (server side) --- */
        memcpy(envelope, SSE_PREFIX, prefix_len);
        memcpy(envelope + prefix_len, payload, payload_len);
        memcpy(envelope + prefix_len + payload_len, SSE_SUFFIX, suffix_len);

        r.total_bytes += envelope_len;

        /*
         * --- Deserialize (client side) ---
         * Find the payload between the prefix and suffix.
         * In real SSE, you'd also parse the "data: " prefix per line,
         * find JSON boundaries, and run a JSON parser. We simulate
         * the minimum: locate payload region and memcpy it out.
         */
        const uint8_t* data_start = envelope + prefix_len;
        size_t data_len = envelope_len - prefix_len - suffix_len;
        memcpy(parse_buf, data_start, data_len);

        /* Simulate a strstr-based SSE line parse (finding '\n\n') */
        volatile uint8_t* volatile scan = envelope;
        while (scan < envelope + envelope_len - 1) {
            if (scan[0] == '\n' && scan[1] == '\n') break;
            scan++;
        }
    }

    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);

    free(envelope);
    free(parse_buf);

    r.total_ns = elapsed_ns(start, end);
    r.frames_per_sec = (double)iterations / (r.total_ns / 1e9);
    r.mb_per_sec = (double)r.total_bytes / (1024.0 * 1024.0) / (r.total_ns / 1e9);
    r.avg_ns_per_frame = r.total_ns / (double)iterations;
    return r;
}

/* ---------- reporting ---------- */

static void print_separator(void) {
    printf("+-----------+----------------+----------------+--------------+--------------+\n");
}

static void print_header(const char* title) {
    printf("\n%s\n", title);
    print_separator();
    printf("| %-9s | %14s | %14s | %12s | %12s |\n",
           "Payload", "Frames/sec", "MB/sec", "Avg ns/frame", "Overhead/msg");
    print_separator();
}

static void print_row(const char* label, BenchResult* r, size_t overhead) {
    printf("| %-9s | %14.0f | %14.2f | %12.1f | %10zu B |\n",
           label, r->frames_per_sec, r->mb_per_sec, r->avg_ns_per_frame, overhead);
}

int main(void) {
    printf("=================================================================\n");
    printf("  MDP Framing Microbenchmark — %d iterations per payload size\n", ITERATIONS);
    printf("=================================================================\n");
    printf("  Measuring: mdp_build_frame() + mdp_parser_feed/next() round-trip\n");
    printf("  Baseline:  simulated HTTP/SSE envelope (data: {JSON}\\n\\n)\n");
    printf("  Clock:     CLOCK_PROCESS_CPUTIME_ID (CPU time, not wall time)\n");

    BenchResult mdp_results[NUM_SIZES];
    BenchResult sse_results[NUM_SIZES];

    for (int s = 0; s < NUM_SIZES; s++) {
        uint8_t* payload = (uint8_t*)malloc(PAYLOAD_SIZES[s]);
        fill_payload(payload, PAYLOAD_SIZES[s]);

        mdp_results[s] = bench_mdp_roundtrip(payload, PAYLOAD_SIZES[s], ITERATIONS);
        sse_results[s] = bench_http_sse_roundtrip(payload, PAYLOAD_SIZES[s], ITERATIONS);

        free(payload);
    }

    /* MDP results */
    print_header("MDP (5-byte header)");
    for (int s = 0; s < NUM_SIZES; s++) {
        print_row(SIZE_LABELS[s], &mdp_results[s], 5);
    }
    print_separator();

    /* HTTP/SSE results */
    print_header("HTTP/SSE (JSON envelope)");
    for (int s = 0; s < NUM_SIZES; s++) {
        size_t overhead = (sizeof(SSE_PREFIX) - 1) + (sizeof(SSE_SUFFIX) - 1);
        print_row(SIZE_LABELS[s], &sse_results[s], overhead);
    }
    print_separator();

    /* Comparison */
    printf("\n--- Comparison ---\n\n");
    printf("| %-9s | %18s | %18s | %10s |\n", "Payload", "MDP ns/frame", "SSE ns/frame", "MDP speedup");
    printf("+-----------+--------------------+--------------------+------------+\n");
    for (int s = 0; s < NUM_SIZES; s++) {
        double speedup = sse_results[s].avg_ns_per_frame / mdp_results[s].avg_ns_per_frame;
        printf("| %-9s | %18.1f | %18.1f | %9.2fx |\n",
               SIZE_LABELS[s],
               mdp_results[s].avg_ns_per_frame,
               sse_results[s].avg_ns_per_frame,
               speedup);
    }
    printf("+-----------+--------------------+--------------------+------------+\n");

    /* Per-message overhead comparison */
    printf("\n--- Per-Message Overhead ---\n\n");
    printf("  MDP:      5 bytes (1 type + 4 length, binary)\n");
    printf("  HTTP/SSE: %zu bytes (\"data: {\\\"type\\\":\\\"chunk\\\",\\\"data\\\":\\\"...\\\"}\\n\\n\")\n",
           (sizeof(SSE_PREFIX) - 1) + (sizeof(SSE_SUFFIX) - 1));
    printf("\n  At 1M messages: MDP adds %.2f KB overhead vs SSE adds %.2f KB overhead\n",
           5.0 * ITERATIONS / 1024.0,
           (double)((sizeof(SSE_PREFIX) - 1) + (sizeof(SSE_SUFFIX) - 1)) * ITERATIONS / 1024.0);

    return 0;
}
