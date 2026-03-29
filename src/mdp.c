#include "mdp.h"

#include <stdlib.h>
#include <string.h>

static uint32_t mdp_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void mdp_write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

void mdp_parser_init(MdpParser* p, size_t max_frame_size) {
    p->buf = NULL;
    p->len = 0;
    p->cap = 0;
    p->max_frame_size = max_frame_size;
    p->has_error = false;
}

void mdp_parser_free(MdpParser* p) {
    free(p->buf);
    p->buf = NULL;
    p->len = 0;
    p->cap = 0;
}

bool mdp_parser_feed(MdpParser* p, const uint8_t* data, size_t data_len) {
    if (p->has_error) {
        return false;
    }

    if (data_len == 0) {
        return true;
    }

    /* Max frame size enforcement should happen in mdp_parser_next on a per-frame basis.
     * We don't limit the total amount of buffered octets here unless it hits system limits. */

    if (p->len + data_len > p->cap) {
        size_t new_cap = p->cap ? p->cap : 4096;
        while (new_cap < p->len + data_len) {
            new_cap *= 2;
        }
        uint8_t* nb = (uint8_t*)realloc(p->buf, new_cap);
        if (!nb) {
            return false;
        }
        p->buf = nb;
        p->cap = new_cap;
    }

    memcpy(p->buf + p->len, data, data_len);
    p->len += data_len;
    return true;
}

bool mdp_parser_next(MdpParser* p, MdpMessage* out) {
    if (p->len < 5) {
        return false;
    }

    uint8_t type = p->buf[0];
    uint32_t payload_len = mdp_be32(p->buf + 1);
    size_t frame_len = 5u + (size_t)payload_len;

    if (p->max_frame_size > 0 && frame_len > p->max_frame_size) {
        p->has_error = true;
        return false;
    }

    if (p->len < frame_len) {
        return false;
    }

    out->type = type;
    out->length = payload_len;
    out->payload = NULL;

    if (payload_len > 0) {
        out->payload = (uint8_t*)malloc(payload_len);
        if (!out->payload) {
            return false;
        }
        memcpy(out->payload, p->buf + 5, payload_len);
    }

    size_t remaining = p->len - frame_len;
    if (remaining > 0) {
        memmove(p->buf, p->buf + frame_len, remaining);
    }
    p->len = remaining;
    return true;
}

void mdp_message_free(MdpMessage* m) {
    free(m->payload);
    m->payload = NULL;
    m->length = 0;
    m->type = 0;
}

bool mdp_build_frame(uint8_t type, const uint8_t* payload, uint32_t payload_len, uint8_t** out_buf, uint32_t* out_len) {
    if (payload_len > 0xFFFFFFF0u) { // Leave room for header
        return false;
    }
    uint32_t frame_len = 5u + payload_len;
    uint8_t* b = (uint8_t*)malloc(frame_len);
    if (!b) {
        return false;
    }

    b[0] = type;
    mdp_write_be32(b + 1, payload_len);
    if (payload_len > 0 && payload) {
        memcpy(b + 5, payload, payload_len);
    }

    *out_buf = b;
    *out_len = frame_len;
    return true;
}

size_t mdp_utf8_safe_prefix(const uint8_t* s, size_t len, size_t max_len) {
    size_t n = (len < max_len) ? len : max_len;
    if (n == 0) {
        return 0;
    }

    /* Find the start of the last character in the range [0, n-1] */
    size_t last_start = n;
    while (last_start > 0 && (s[last_start - 1] & 0xC0) == 0x80) {
        last_start--;
    }

    if (last_start == 0) {
        /* If the whole range is continuation bytes (invalid UTF-8 or split across boundary),
         * we return 0. */
        return 0;
    }

    uint8_t lead = s[last_start - 1];
    size_t expected_len = 0;
    if ((lead & 0x80) == 0) expected_len = 1;
    else if ((lead & 0xE0) == 0xC0) expected_len = 2;
    else if ((lead & 0xF0) == 0xE0) expected_len = 3;
    else if ((lead & 0xF8) == 0xF0) expected_len = 4;
    else {
        /* Invalid lead byte, skip it */
        return last_start - 1;
    }

    if (last_start - 1 + expected_len <= n) {
        /* The last character fits in the range */
        return n;
    } else {
        /* The last character is truncated, return the prefix before it */
        return last_start - 1;
    }
}

bool mdp_validate_request_json(const uint8_t* payload, uint32_t payload_len) {
    if (payload_len < 2) return false;
    if (payload[0] != '{' || payload[payload_len - 1] != '}') return false;

    char* str = (char*)malloc(payload_len + 1);
    if (!str) return false;
    memcpy(str, payload, payload_len);
    str[payload_len] = '\0';
    
    // Very basic check for required keys. In a real impl, use a JSON parser.
    bool valid = true;
    if (!strstr(str, "\"model\"") || !strstr(str, "\"prompt\"")) {
        valid = false;
    }
    
    free(str);
    return valid;
}
