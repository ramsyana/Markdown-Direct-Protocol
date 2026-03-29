#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdpMsgType {
    MDP_MSG_REQUEST = 0x00,
    MDP_MSG_METADATA = 0x01,
    MDP_MSG_MARKDOWN_CHUNK = 0x02,
    MDP_MSG_END_OF_RESPONSE = 0x03,
    MDP_MSG_ERROR = 0x04
} MdpMsgType;

typedef struct MdpMessage {
    uint8_t type;
    uint32_t length;
    uint8_t* payload;
} MdpMessage;

typedef struct MdpParser {
    uint8_t* buf;
    size_t len;
    size_t cap;
    size_t max_frame_size;
    bool has_error;
} MdpParser;

void mdp_parser_init(MdpParser* p, size_t max_frame_size);
void mdp_parser_free(MdpParser* p);

bool mdp_parser_feed(MdpParser* p, const uint8_t* data, size_t data_len);

bool mdp_parser_next(MdpParser* p, MdpMessage* out);
void mdp_message_free(MdpMessage* m);

bool mdp_build_frame(uint8_t type, const uint8_t* payload, uint32_t payload_len, uint8_t** out_buf, uint32_t* out_len);

size_t mdp_utf8_safe_prefix(const uint8_t* s, size_t len, size_t max_len);
bool mdp_validate_request_json(const uint8_t* payload, uint32_t payload_len);

#ifdef __cplusplus
}
#endif
