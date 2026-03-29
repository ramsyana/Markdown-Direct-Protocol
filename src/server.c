#include <msquic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "mdp.h"

#define MDP_ALPN "mdp/1.0"

typedef enum StreamState {
    STATE_WAIT_REQUEST,
    STATE_SEND_META,
    STATE_SEND_CHUNKS,
    STATE_SEND_END,
    STATE_DONE
} StreamState;

typedef struct StreamCtx {
    HQUIC Stream;
    struct ServerCtx* S;
    MdpParser Parser;
    StreamState State;
    const uint8_t* Payload;
    uint32_t PayloadTotal;
    uint32_t PayloadOff;
} StreamCtx;

typedef struct ServerCtx {
    const QUIC_API_TABLE* Api;
    HQUIC Registration;
    HQUIC Configuration;
    HQUIC Listener;
} ServerCtx;

static void StreamSendFrame(ServerCtx* S, HQUIC Stream, uint8_t type, const uint8_t* payload, uint32_t payload_len, bool fin) {
    uint8_t* frame = NULL;
    uint32_t frame_len = 0;
    if (!mdp_build_frame(type, payload, payload_len, &frame, &frame_len)) {
        S->Api->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND | QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0x04);
        return;
    }

    QUIC_BUFFER* qb = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER));
    if (!qb) {
        free(frame);
        S->Api->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND | QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0x04);
        return;
    }

    qb->Length = frame_len;
    qb->Buffer = frame;

    QUIC_SEND_FLAGS flags = fin ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE;
    QUIC_STATUS st = S->Api->StreamSend(Stream, qb, 1, flags, qb);
    if (QUIC_FAILED(st)) {
        free(frame);
        free(qb);
        S->Api->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND | QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0x04);
    }
}

static void StreamPump(StreamCtx* C) {
    if (C->State == STATE_DONE) return;

    if (C->State == STATE_SEND_META) {
        const char* meta = "{\"model\":\"demo\",\"request_id\":null,\"estimated_tokens\":null}";
        StreamSendFrame(C->S, C->Stream, MDP_MSG_METADATA, (const uint8_t*)meta, (uint32_t)strlen(meta), false);
        C->State = STATE_SEND_CHUNKS;
        return;
    }

    if (C->State == STATE_SEND_CHUNKS) {
        if (C->PayloadOff < C->PayloadTotal) {
            uint32_t chunk_max = 32;
            uint32_t rem = C->PayloadTotal - C->PayloadOff;
            uint32_t take = (rem < chunk_max) ? rem : chunk_max;
            size_t safe = mdp_utf8_safe_prefix(C->Payload + C->PayloadOff, rem, take);
            if (safe == 0 && rem > 0) {
                // Invalid UTF-8 or lead byte at the very end of valid range.
                // In this demo, we treat it as an internal error.
                C->S->Api->StreamShutdown(C->Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND | QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0x04);
                C->State = STATE_DONE;
                return;
            }
            StreamSendFrame(C->S, C->Stream, MDP_MSG_MARKDOWN_CHUNK, C->Payload + C->PayloadOff, (uint32_t)safe, false);
            C->PayloadOff += (uint32_t)safe;
            return;
        } else {
            C->State = STATE_SEND_END;
        }
    }

    if (C->State == STATE_SEND_END) {
        const char* end = "{\"tokens\":null}";
        StreamSendFrame(C->S, C->Stream, MDP_MSG_END_OF_RESPONSE, (const uint8_t*)end, (uint32_t)strlen(end), true);
        C->State = STATE_DONE;
        return;
    }
}

static QUIC_STATUS QUIC_API StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    StreamCtx* C = (StreamCtx*)Context;
    ServerCtx* S = C->S;

    switch (Event->Type) {
        case QUIC_STREAM_EVENT_START_COMPLETE:
            return QUIC_STATUS_SUCCESS;

        case QUIC_STREAM_EVENT_RECEIVE: {
            for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; i++) {
                if (!mdp_parser_feed(&C->Parser, Event->RECEIVE.Buffers[i].Buffer, Event->RECEIVE.Buffers[i].Length)) {
                    S->Api->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND | QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0x04);
                    return QUIC_STATUS_SUCCESS;
                }
            }

            for (;;) {
                MdpMessage m;
                if (!mdp_parser_next(&C->Parser, &m)) {
                    break;
                }

                if (m.type == MDP_MSG_REQUEST) {
                    if (C->State != STATE_WAIT_REQUEST) {
                        mdp_message_free(&m);
                        S->Api->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND | QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0x01);
                        return QUIC_STATUS_SUCCESS;
                    }
                    
                    if (!mdp_validate_request_json(m.payload, m.length)) {
                        mdp_message_free(&m);
                        S->Api->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND | QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0x01);
                        return QUIC_STATUS_SUCCESS;
                    }

                    C->State = STATE_SEND_META;
                    C->Payload = (const uint8_t*)"# MDP demo\n\n**Hello** from MsQuic.\n";
                    C->PayloadTotal = (uint32_t)strlen((const char*)C->Payload);
                    C->PayloadOff = 0;
                    
                    StreamPump(C);
                }

                mdp_message_free(&m);
            }

            return QUIC_STATUS_SUCCESS;
        }

        case QUIC_STREAM_EVENT_SEND_COMPLETE: {
            QUIC_BUFFER* qb = (QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext;
            if (qb) {
                free(qb->Buffer);
                free(qb);
            }
            StreamPump(C);
            return QUIC_STATUS_SUCCESS;
        }

        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            return QUIC_STATUS_SUCCESS;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            mdp_parser_free(&C->Parser);
            free(C);
            return QUIC_STATUS_SUCCESS;

        default:
            return QUIC_STATUS_SUCCESS;
    }
}

static QUIC_STATUS QUIC_API ConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    ServerCtx* S = (ServerCtx*)Context;

    switch (Event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
            StreamCtx* sc = (StreamCtx*)calloc(1, sizeof(StreamCtx));
            if (!sc) {
                S->Api->ConnectionShutdown(Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0x04);
                return QUIC_STATUS_SUCCESS;
            }
            mdp_parser_init(&sc->Parser, 1024 * 1024); // 1MB max frame size
            sc->Stream = Event->PEER_STREAM_STARTED.Stream;
            sc->S = S;
            sc->State = STATE_WAIT_REQUEST;

            S->Api->SetCallbackHandler(sc->Stream, (void*)StreamCallback, sc);
            return QUIC_STATUS_SUCCESS;
        }

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            S->Api->ConnectionClose(Connection);
            return QUIC_STATUS_SUCCESS;

        default:
            return QUIC_STATUS_SUCCESS;
    }
}

static QUIC_STATUS QUIC_API ListenerCallback(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event) {
    ServerCtx* S = (ServerCtx*)Context;
    (void)Listener;

    switch (Event->Type) {
        case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
            S->Api->SetCallbackHandler(Event->NEW_CONNECTION.Connection, (void*)ConnectionCallback, S);
            return S->Api->ConnectionSetConfiguration(Event->NEW_CONNECTION.Connection, S->Configuration);
        }
        default:
            return QUIC_STATUS_SUCCESS;
    }
}

static void usage(const char* argv0) {
    fprintf(stderr, "Usage: %s <listen_port> <cert_pem> <key_pem>\n", argv0);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        usage(argv[0]);
        return 2;
    }

    uint16_t port = (uint16_t)atoi(argv[1]);
    const char* cert = argv[2];
    const char* key = argv[3];

    ServerCtx S;
    memset(&S, 0, sizeof(S));

    if (QUIC_FAILED(MsQuicOpen2(&S.Api))) {
        fprintf(stderr, "MsQuicOpen2 failed\n");
        return 1;
    }

    QUIC_REGISTRATION_CONFIG reg = {"mdp", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    if (QUIC_FAILED(S.Api->RegistrationOpen(&reg, &S.Registration))) {
        fprintf(stderr, "RegistrationOpen failed\n");
        MsQuicClose(S.Api);
        return 1;
    }

    QUIC_BUFFER alpn = {(uint32_t)strlen(MDP_ALPN), (uint8_t*)MDP_ALPN};

    QUIC_SETTINGS settings;
    memset(&settings, 0, sizeof(settings));
    settings.IsSet.PeerBidiStreamCount = 1;
    settings.PeerBidiStreamCount = 100;

    if (QUIC_FAILED(S.Api->ConfigurationOpen(S.Registration, &alpn, 1, &settings, sizeof(settings), NULL, &S.Configuration))) {
        fprintf(stderr, "ConfigurationOpen failed\n");
        S.Api->RegistrationClose(S.Registration);
        MsQuicClose(S.Api);
        return 1;
    }

    QUIC_CREDENTIAL_CONFIG cred;
    memset(&cred, 0, sizeof(cred));
    cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    QUIC_CERTIFICATE_FILE cert_file = {(char*)cert, (char*)key};
    cred.CertificateFile = &cert_file;

    if (QUIC_FAILED(S.Api->ConfigurationLoadCredential(S.Configuration, &cred))) {
        fprintf(stderr, "ConfigurationLoadCredential failed\n");
        S.Api->ConfigurationClose(S.Configuration);
        S.Api->RegistrationClose(S.Registration);
        MsQuicClose(S.Api);
        return 1;
    }

    if (QUIC_FAILED(S.Api->ListenerOpen(S.Registration, ListenerCallback, &S, &S.Listener))) {
        fprintf(stderr, "ListenerOpen failed\n");
        S.Api->ConfigurationClose(S.Configuration);
        S.Api->RegistrationClose(S.Registration);
        MsQuicClose(S.Api);
        return 1;
    }

    QUIC_ADDR addr;
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&addr, port);

    if (QUIC_FAILED(S.Api->ListenerStart(S.Listener, &alpn, 1, &addr))) {
        fprintf(stderr, "ListenerStart failed\n");
        S.Api->ListenerClose(S.Listener);
        S.Api->ConfigurationClose(S.Configuration);
        S.Api->RegistrationClose(S.Registration);
        MsQuicClose(S.Api);
        return 1;
    }

    printf("mdp_server listening on UDP/%u (ALPN %s)\n", port, MDP_ALPN);
    fflush(stdout);

    for (;;) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
    }

    return 0;
}
