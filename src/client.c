#include <msquic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include "mdp.h"

#define MDP_ALPN "mdp/1.0"

#include <pthread.h>
#include <time.h>

typedef struct ClientCtx {
    const QUIC_API_TABLE* Api;
    HQUIC Registration;
    HQUIC Configuration;
    HQUIC Connection;
    HQUIC Stream;
    MdpParser Parser;
    bool Done;
    pthread_mutex_t Mutex;
    pthread_cond_t Cond;
} ClientCtx;

static QUIC_STATUS QUIC_API StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    ClientCtx* C = (ClientCtx*)Context;

    switch (Event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; i++) {
                if (!mdp_parser_feed(&C->Parser, Event->RECEIVE.Buffers[i].Buffer, Event->RECEIVE.Buffers[i].Length)) {
                    C->Api->ConnectionShutdown(C->Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0x04);
                    return QUIC_STATUS_SUCCESS;
                }
            }

            for (;;) {
                MdpMessage m;
                if (!mdp_parser_next(&C->Parser, &m)) {
                    break;
                }

                if (m.type == MDP_MSG_MARKDOWN_CHUNK) {
                    fwrite(m.payload, 1, m.length, stdout);
                    fflush(stdout);
                } else if (m.type == MDP_MSG_ERROR) {
                    fwrite(m.payload, 1, m.length, stderr);
                    fputc('\n', stderr);
                    pthread_mutex_lock(&C->Mutex);
                    C->Done = true;
                    pthread_cond_signal(&C->Cond);
                    pthread_mutex_unlock(&C->Mutex);
                    C->Api->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
                    C->Api->ConnectionShutdown(C->Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
                } else if (m.type == MDP_MSG_END_OF_RESPONSE) {
                    pthread_mutex_lock(&C->Mutex);
                    C->Done = true;
                    pthread_cond_signal(&C->Cond);
                    pthread_mutex_unlock(&C->Mutex);
                    C->Api->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
                    C->Api->ConnectionShutdown(C->Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
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
            return QUIC_STATUS_SUCCESS;
        }

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            return QUIC_STATUS_SUCCESS;

        default:
            return QUIC_STATUS_SUCCESS;
    }
}

static QUIC_STATUS QUIC_API ConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    ClientCtx* C = (ClientCtx*)Context;
    (void)Connection;

    switch (Event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            C->Api->ConnectionClose(C->Connection);
            C->Connection = NULL;
            return QUIC_STATUS_SUCCESS;

        default:
            return QUIC_STATUS_SUCCESS;
    }
}

static void usage(const char* argv0) {
    fprintf(stderr, "Usage: %s <host> <port> <request_json>\n", argv0);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        usage(argv[0]);
        return 2;
    }

    const char* host = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char* req_json = argv[3];

    ClientCtx C;
    memset(&C, 0, sizeof(C));
    mdp_parser_init(&C.Parser, 10 * 1024 * 1024); // 10MB max frame size

    if (QUIC_FAILED(MsQuicOpen2(&C.Api))) {
        fprintf(stderr, "MsQuicOpen2 failed\n");
        return 1;
    }

    pthread_mutex_init(&C.Mutex, NULL);
    pthread_cond_init(&C.Cond, NULL);

    QUIC_REGISTRATION_CONFIG reg = {"mdp", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    if (QUIC_FAILED(C.Api->RegistrationOpen(&reg, &C.Registration))) {
        fprintf(stderr, "RegistrationOpen failed\n");
        MsQuicClose(C.Api);
        return 1;
    }

    QUIC_BUFFER alpn = {(uint32_t)strlen(MDP_ALPN), (uint8_t*)MDP_ALPN};

    QUIC_SETTINGS settings;
    memset(&settings, 0, sizeof(settings));
    settings.IsSet.IdleTimeoutMs = 1;
    settings.IdleTimeoutMs = 60000;

    if (QUIC_FAILED(C.Api->ConfigurationOpen(C.Registration, &alpn, 1, &settings, sizeof(settings), NULL, &C.Configuration))) {
        fprintf(stderr, "ConfigurationOpen failed\n");
        C.Api->RegistrationClose(C.Registration);
        MsQuicClose(C.Api);
        return 1;
    }

    QUIC_CREDENTIAL_CONFIG cred;
    memset(&cred, 0, sizeof(cred));
    cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

    if (QUIC_FAILED(C.Api->ConfigurationLoadCredential(C.Configuration, &cred))) {
        fprintf(stderr, "ConfigurationLoadCredential failed\n");
        C.Api->ConfigurationClose(C.Configuration);
        C.Api->RegistrationClose(C.Registration);
        MsQuicClose(C.Api);
        return 1;
    }

    if (QUIC_FAILED(C.Api->ConnectionOpen(C.Registration, ConnectionCallback, &C, &C.Connection))) {
        fprintf(stderr, "ConnectionOpen failed\n");
        C.Api->ConfigurationClose(C.Configuration);
        C.Api->RegistrationClose(C.Registration);
        MsQuicClose(C.Api);
        return 1;
    }

    if (QUIC_FAILED(C.Api->ConnectionStart(C.Connection, C.Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, host, port))) {
        fprintf(stderr, "ConnectionStart failed\n");
        C.Api->ConnectionClose(C.Connection);
        C.Api->ConfigurationClose(C.Configuration);
        C.Api->RegistrationClose(C.Registration);
        MsQuicClose(C.Api);
        return 1;
    }

    if (QUIC_FAILED(C.Api->StreamOpen(C.Connection, QUIC_STREAM_OPEN_FLAG_NONE, StreamCallback, &C, &C.Stream))) {
        fprintf(stderr, "StreamOpen failed\n");
        C.Api->ConnectionShutdown(C.Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0x04);
        return 1;
    }

    if (QUIC_FAILED(C.Api->StreamStart(C.Stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
        fprintf(stderr, "StreamStart failed\n");
        C.Api->StreamClose(C.Stream);
        C.Api->ConnectionShutdown(C.Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0x04);
        return 1;
    }

    uint8_t* frame = NULL;
    uint32_t frame_len = 0;
    if (!mdp_build_frame(MDP_MSG_REQUEST, (const uint8_t*)req_json, (uint32_t)strlen(req_json), &frame, &frame_len)) {
        fprintf(stderr, "Failed to build request frame\n");
        return 1;
    }

    QUIC_BUFFER* qb = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER));
    if (!qb) {
        free(frame);
        return 1;
    }
    qb->Buffer = frame;
    qb->Length = frame_len;

    if (QUIC_FAILED(C.Api->StreamSend(C.Stream, qb, 1, QUIC_SEND_FLAG_NONE, qb))) {
        fprintf(stderr, "StreamSend failed\n");
        free(frame);
        free(qb);
        return 1;
    }


    while (!C.Done) {
        pthread_mutex_lock(&C.Mutex);
        if (C.Done) {
            pthread_mutex_unlock(&C.Mutex);
            break;
        }
        
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 10; // 10 second timeout

        int rc = pthread_cond_timedwait(&C.Cond, &C.Mutex, &ts);
        pthread_mutex_unlock(&C.Mutex);
        
        if (rc != 0) {
            fprintf(stderr, "Timed out waiting for response\n");
            break;
        }
    }

    if (C.Stream) {
        C.Api->StreamClose(C.Stream);
    }

    mdp_parser_free(&C.Parser);
    C.Api->ConfigurationClose(C.Configuration);
    C.Api->RegistrationClose(C.Registration);
    MsQuicClose(C.Api);
    return 0;
}
