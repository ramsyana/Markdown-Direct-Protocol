# MDP — Markdown Direct Protocol
**Version 1.1** (March 2026)

## 1. Goals
- Stream raw UTF-8 markdown from an LLM with minimum CPU overhead between token generation and network write.
- Per-message overhead: exactly 5 bytes.
- No parsing or transformation of markdown chunks on the server.
- Designed for internal inference infrastructure (server-to-server, within a controlled network). Not intended as a client-facing API protocol.

## 2. Use Cases
MDP is intended for internal inference transport where both ends are controlled and throughput is the primary concern:

- Inference server to API gateway.
- Inference cluster to streaming backend.
- Any server-to-server LLM token stream where HTTP overhead is measurable and undesirable.

MDP is not intended for browser clients or any environment where existing HTTP/3 infrastructure provides sufficient performance.

## 3. Why MDP? (Motivation & Performance)

In high-volume internal inference setups, every CPU cycle and byte counts. Standard HTTP/3 with SSE (Server-Sent Events) or gRPC adds measurable overhead in serialization, header bloat, and parsing:

- **Minimal Framing**: MDP uses exactly 5 bytes per frame header. Compare this to the verbosity of HTTP/1.1 or the complex frame types of HTTP/2 and 3.
- **Zero Serialization Hell**: MDP streams raw markdown. No JSON escaping or base64 overhead.
- **Internal Efficiency**: Optimized for backend-to-backend communication where you control both ends and want to squeeze out every bit of throughput between the model server and the gateway.

### Overhead Comparison (Per Frame)

| Feature             | MDP           | HTTP/3 + SSE      | gRPC (Protobuf)   |
|---------------------|---------------|-------------------|-------------------|
| **Header Size**     | 5 Bytes       | ~10-30+ Bytes     | 5-10+ Bytes       |
| **Payload Format**  | Raw UTF-8     | JSON / Escaped    | Binary (encoded)  |
| **Framing Logic**   | Fixed-length  | Line-based (SSE)  | Message-based     |
| **Parsing Cost**    | Near Zero     | High (JSON/Str)   | Medium (D-series) |

### Benchmarks (Microbenchmark)

Measuring 1M message round-trip (Build + Parse) on a single core:

| Payload Size | MDP Speedup vs SSE | MDP Overhead (1M msg) | SSE Overhead (1M msg) |
|--------------|--------------------|-----------------------|-----------------------|
| **32 Bytes** | **1.51x** faster   | 4.8 MB                | 33.2 MB               |
| **512 Bytes**| **1.36x** faster   | 4.8 MB                | 33.2 MB               |

*Note: For very large payloads (>4KB), the speedup narrows as raw data transfer (memcpy) dominates framing overhead.*

## 4. Transport
- **Protocol**: IETF QUIC v1 (RFC 9000)
- **ALPN**: `mdp/1.0`
- **Streams**: One bidirectional QUIC stream per completion.
- **0-RTT**: Permitted. *Note: 0-RTT does not provide anti-replay protection. Applications MUST ensure request idempotency or implement higher-level anti-replay if 0-RTT is enabled.*
- **TLS**: Mandatory (QUIC built-in TLS 1.3).

## 4. Wire Format
Every message uses a fixed 5-byte header followed by a variable-length payload:

| Offset | Field   | Size     | Type                | Description             |
|--------|---------|----------|---------------------|-------------------------|
| 0      | Type    | 1 byte   | uint8               | Message type (see §5)   |
| 1–4    | Length  | 4 bytes  | uint32 (big-endian) | Payload length in bytes |
| 5+     | Payload | variable | raw bytes           | Depends on message type |

**Maximum Frame Size**: Implementations SHOULD NOT send frames exceeding 16 MB. Receivers MUST reject frames exceeding their local buffer limits with `PROTOCOL_VIOLATION`.

## 5. Message Types

| Type | Name             | Direction | Payload                           | Requirement             |
|------|------------------|-----------|-----------------------------------|-------------------------|
| 0x00 | Request          | C → S     | UTF-8 JSON (see §6)               | Exactly one per stream  |
| 0x01 | Metadata         | S → C     | UTF-8 JSON (see §7)               | Optional; at most once  |
| 0x02 | Markdown Chunk   | S → C     | Raw UTF-8 bytes                   | Zero or more            |
| 0x03 | End of Response  | S → C     | UTF-8 JSON (see §8)               | Exactly one             |
| 0x04 | Error            | S → C     | UTF-8 JSON or plain text          | Terminal message        |

Receivers MUST ignore unknown message types.

## 6. Request Schema
```json
{
  "model":       "string",          // required
  "prompt":      "string",          // required
  "max_tokens":  integer | null,    // null or omitted = unlimited
  "temperature": number,            // optional
  "top_p":       number,            // optional
  "stop":        ["string"] | null, // optional stop sequences
  "metadata":    { ... }            // optional vendor-specific fields
}
```

## 7. Metadata Schema
```json
{
  "model":            "string",        // required
  "request_id":       "string" | null, // optional
  "estimated_tokens": integer | null,  // optional
  "extra":            { ... }          // optional vendor-specific fields
}
```

## 8. End of Response Schema
```json
{
  "tokens":      integer | null,    // total tokens generated
  "finish_reason": "string" | null, // "stop", "length", "error"
  "extra":         { ... }          // optional vendor-specific fields
}
```

## 9. Server Behaviour
- **Single request per stream**: The server MUST process exactly one Request per stream. A second Request on the same stream MUST be treated as a PROTOCOL_VIOLATION (error code 0x01) and the stream MUST be closed immediately.
- **Chunk integrity**: The server MUST send Markdown Chunk payloads as raw UTF-8 bytes, unmodified from LLM output. No parsing or transformation is permitted.
- **UTF-8 boundaries**: Every Markdown Chunk MUST be a complete, valid UTF-8 sequence. The server MUST NOT split a multi-byte codepoint across chunk boundaries.
- **Chunking**: Size-based only. Recommended chunk size: 512–4096 bytes, or flush when the QUIC layer is ready to send.
- **Flow control**: The server MUST respect QUIC stream flow control. Implementations SHOULD drive production/sending via QUIC "writable" or "send complete" signals to prevent memory bloat if the client stops reading.
- **Error Handling**: Fatal protocol errors (e.g. malformed header) MUST result in an immediate `PROTOCOL_VIOLATION` (0x01) via QUIC `RESET_STREAM`. Application-level errors (e.g. model busy) SHOULD be communicated via an `Error` frame (0x04) followed by graceful shutdown if possible, or `RESET_STREAM` if not.

## 10. Error Codes
Used in QUIC `RESET_STREAM` or `CONNECTION_CLOSE`:

| Code | Name               | Meaning                                        |
|------|--------------------|------------------------------------------------|
| 0x01 | PROTOCOL_VIOLATION | Invalid message, bad format, or second Request |
| 0x02 | RATE_LIMIT         | Too many concurrent requests                   |
| 0x03 | MODEL_UNAVAILABLE  | Requested model is not loaded                  |
| 0x04 | INTERNAL_ERROR     | Server-side failure                            |

## 11. Example: Markdown Chunk Packet
Payload: `**Hello**` (9 bytes)

```
02 00 00 00 09 2A 2A 48 65 6C 6C 6F 2A 2A
│  └──────────────┘  └────────────────────┘
│    length = 9        payload = "**Hello**"
Type = 0x02 (Markdown Chunk)
```

---

## Reference Implementation (MsQuic)

This repository includes a minimal **C** reference implementation using **MsQuic**:

- `mdp_server`: QUIC listener (ALPN `mdp/1.0`) that accepts a single `Request` frame per stream and streams back `Metadata`, `Markdown Chunk`, and `End of Response`.
- `mdp_client`: connects to the server, sends a `Request`, and prints streamed Markdown chunks to stdout.

### Rust Example

A production-ready Rust client implementation is available in [`examples/rust-client/`](./examples/rust-client/):

- Uses the **Quinn** (async QUIC) library.
- Supports ALPN `mdp/1.0`.
- Streams chunks to stdout in real-time.

### Benchmarks

Run the framing performance benchmark:

```bash
# Compile and run (requires cmake)
./benchmarks/run_benchmarks.sh
```

### Build (Linux)

Prerequisites:

- MsQuic development files (`msquic.h` and `libmsquic.so`)
- CMake + a C compiler

Build:

```bash
cmake -S . -B build
cmake --build build
```

If MsQuic isn’t in a standard include/library path, set:

```bash
cmake -S . -B build \
  -DMSQUIC_INCLUDE_DIR=/path/to/msquic/include \
  -DMSQUIC_LIBRARY=/path/to/libmsquic.so
```

### Run

Generate a self-signed certificate:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout key.pem -out cert.pem -days 365 -subj "/CN=localhost"
```

Start the server:

```bash
./build/mdp_server 4443 cert.pem key.pem
```

Run the client:

```bash
./build/mdp_client 127.0.0.1 4443 '{"model":"demo","prompt":"hi"}'
```
