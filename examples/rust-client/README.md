# MDP Rust Client Example

A minimal [MDP](../../README.md) client written in Rust using [quinn](https://github.com/quinn-rs/quinn) (async QUIC).

## Build

```bash
cargo build --release
```

## Usage

Start the MDP reference server first:

```bash
# From the project root
./build/mdp_server 4443 cert.pem key.pem
```

Then run the Rust client:

```bash
cargo run -- --host 127.0.0.1 --port 4443 --model demo --prompt "hello"
```

Markdown chunks stream to stdout in real-time. Metadata and status messages go to stderr.

## Options

```
--host          Server hostname or IP [default: 127.0.0.1]
--port          Server UDP port [default: 4443]
--model         Model name (required)
--prompt        Prompt text (required)
--temperature   Optional temperature parameter
```

## Notes

- Uses `dangerous()` TLS configuration to skip certificate verification (for dev/self-signed certs)
- Implements the full MDP wire format: 5-byte header (1 type + 4 length BE) + payload
- Handles all MDP message types: Request, Metadata, Markdown Chunk, End of Response, Error
- Unknown message types are ignored per spec
