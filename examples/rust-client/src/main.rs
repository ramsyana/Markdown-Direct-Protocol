//! MDP (Markdown Direct Protocol) client in Rust
//!
//! Connects to an MDP server over QUIC, sends a Request frame,
//! and streams Markdown chunks to stdout.
//!
//! Usage:
//!   cargo run -- --host 127.0.0.1 --port 4443 --model demo --prompt "hello"

use anyhow::{bail, Context, Result};
use clap::Parser;
use quinn::crypto::rustls::QuicClientConfig;
use std::sync::Arc;

// ── MDP constants ──────────────────────────────────────────────

const MDP_ALPN: &[u8] = b"mdp/1.0";
const MDP_HEADER_LEN: usize = 5;

const MSG_REQUEST: u8 = 0x00;
const MSG_METADATA: u8 = 0x01;
const MSG_MARKDOWN_CHUNK: u8 = 0x02;
const MSG_END_OF_RESPONSE: u8 = 0x03;
const MSG_ERROR: u8 = 0x04;

// ── MDP frame helpers ──────────────────────────────────────────

/// Build a 5-byte-header MDP frame: [type(1)][length(4 BE)][payload]
fn build_frame(msg_type: u8, payload: &[u8]) -> Vec<u8> {
    let len = payload.len() as u32;
    let mut frame = Vec::with_capacity(MDP_HEADER_LEN + payload.len());
    frame.push(msg_type);
    frame.extend_from_slice(&len.to_be_bytes());
    frame.extend_from_slice(payload);
    frame
}

/// Read exactly one MDP frame from the receive stream.
/// Returns (type, payload) or None on clean stream close.
async fn read_frame(recv: &mut quinn::RecvStream) -> Result<Option<(u8, Vec<u8>)>> {
    // Read the 5-byte header
    let mut header = [0u8; MDP_HEADER_LEN];
    match recv.read_exact(&mut header).await {
        Ok(()) => {}
        Err(quinn::ReadExactError::FinishedEarly(_)) => return Ok(None),
        Err(e) => return Err(e.into()),
    }

    let msg_type = header[0];
    let length = u32::from_be_bytes([header[1], header[2], header[3], header[4]]) as usize;

    // Enforce a sane limit (16 MB as per spec recommendation)
    if length > 16 * 1024 * 1024 {
        bail!("frame too large: {length} bytes (max 16 MB)");
    }

    // Read the payload
    let mut payload = vec![0u8; length];
    if length > 0 {
        recv.read_exact(&mut payload)
            .await
            .context("failed to read frame payload")?;
    }

    Ok(Some((msg_type, payload)))
}

// ── CLI ────────────────────────────────────────────────────────

#[derive(Parser)]
#[command(name = "mdp-client", about = "MDP client — streams markdown over QUIC")]
struct Cli {
    /// Server hostname or IP
    #[arg(long, default_value = "127.0.0.1")]
    host: String,

    /// Server UDP port
    #[arg(long, default_value_t = 4443)]
    port: u16,

    /// Model name
    #[arg(long)]
    model: String,

    /// Prompt text
    #[arg(long)]
    prompt: String,

    /// Optional temperature
    #[arg(long)]
    temperature: Option<f64>,
}

// ── Main ───────────────────────────────────────────────────────

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();

    // ── TLS config (skip certificate verification for dev/self-signed) ──
    let mut crypto = rustls::ClientConfig::builder()
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(SkipServerVerification))
        .with_no_client_auth();
    crypto.alpn_protocols = vec![MDP_ALPN.to_vec()];

    let mut transport = quinn::TransportConfig::default();
    transport.max_idle_timeout(Some(
        quinn::IdleTimeout::try_from(std::time::Duration::from_secs(30)).unwrap(),
    ));

    let mut client_config = quinn::ClientConfig::new(Arc::new(
        QuicClientConfig::try_from(crypto).context("failed to build QUIC client config")?,
    ));
    client_config.transport_config(Arc::new(transport));

    // Set ALPN
    // (quinn gets ALPN from the rustls config, which we set below via the crypto config)

    // ── Endpoint ──
    let mut endpoint = quinn::Endpoint::client("0.0.0.0:0".parse()?)?;
    endpoint.set_default_client_config(client_config);

    // ── Connect ──
    let server_addr = format!("{}:{}", cli.host, cli.port).parse()?;
    let connection = endpoint
        .connect(server_addr, &cli.host)?
        .await
        .context("QUIC connection failed")?;

    eprintln!(
        "connected to {} (protocol: {:?})",
        server_addr,
        connection
            .handshake_data()
            .and_then(|h| h.downcast::<quinn::crypto::rustls::HandshakeData>().ok())
            .and_then(|h| h.protocol.as_ref().map(|p| String::from_utf8_lossy(p).to_string()))
            .unwrap_or_else(|| "unknown".to_string())
    );

    // ── Open bidirectional stream ──
    let (mut send, mut recv) = connection
        .open_bi()
        .await
        .context("failed to open bidirectional stream")?;

    // ── Build and send the Request frame ──
    let request_json = build_request_json(&cli);
    let request_frame = build_frame(MSG_REQUEST, request_json.as_bytes());
    send.write_all(&request_frame)
        .await
        .context("failed to send request frame")?;

    // ── Read response frames ──
    loop {
        match read_frame(&mut recv).await? {
            None => {
                // Stream closed cleanly
                break;
            }
            Some((msg_type, payload)) => match msg_type {
                MSG_METADATA => {
                    let meta = String::from_utf8_lossy(&payload);
                    eprintln!("metadata: {meta}");
                }
                MSG_MARKDOWN_CHUNK => {
                    // Stream raw markdown to stdout
                    use std::io::Write;
                    std::io::stdout().write_all(&payload)?;
                    std::io::stdout().flush()?;
                }
                MSG_END_OF_RESPONSE => {
                    let end = String::from_utf8_lossy(&payload);
                    eprintln!("\n--- end of response: {end}");
                    break;
                }
                MSG_ERROR => {
                    let err = String::from_utf8_lossy(&payload);
                    eprintln!("server error: {err}");
                    break;
                }
                _ => {
                    // Spec says: receivers MUST ignore unknown message types
                    eprintln!("ignoring unknown message type: 0x{msg_type:02x}");
                }
            },
        }
    }

    // ── Cleanup ──
    send.finish()?;
    connection.close(0u32.into(), b"done");
    endpoint.wait_idle().await;

    Ok(())
}

// ── Helpers ────────────────────────────────────────────────────

fn build_request_json(cli: &Cli) -> String {
    let mut json = format!(
        r#"{{"model":"{}","prompt":"{}""#,
        escape_json(&cli.model),
        escape_json(&cli.prompt)
    );
    if let Some(temp) = cli.temperature {
        json.push_str(&format!(r#","temperature":{temp}"#));
    }
    json.push('}');
    json
}

fn escape_json(s: &str) -> String {
    s.replace('\\', "\\\\")
        .replace('"', "\\\"")
        .replace('\n', "\\n")
        .replace('\r', "\\r")
        .replace('\t', "\\t")
}

// ── TLS: skip certificate verification (dev only) ─────────────

#[derive(Debug)]
struct SkipServerVerification;

impl rustls::client::danger::ServerCertVerifier for SkipServerVerification {
    fn verify_server_cert(
        &self,
        _end_entity: &rustls::pki_types::CertificateDer<'_>,
        _intermediates: &[rustls::pki_types::CertificateDer<'_>],
        _server_name: &rustls::pki_types::ServerName<'_>,
        _ocsp_response: &[u8],
        _now: rustls::pki_types::UnixTime,
    ) -> Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::danger::ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &rustls::pki_types::CertificateDer<'_>,
        _dss: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn verify_tls13_signature(
        &self,
        _message: &[u8],
        _cert: &rustls::pki_types::CertificateDer<'_>,
        _dss: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        vec![
            rustls::SignatureScheme::RSA_PKCS1_SHA256,
            rustls::SignatureScheme::RSA_PKCS1_SHA384,
            rustls::SignatureScheme::RSA_PKCS1_SHA512,
            rustls::SignatureScheme::ECDSA_NISTP256_SHA256,
            rustls::SignatureScheme::ECDSA_NISTP384_SHA384,
            rustls::SignatureScheme::ECDSA_NISTP521_SHA512,
            rustls::SignatureScheme::RSA_PSS_SHA256,
            rustls::SignatureScheme::RSA_PSS_SHA384,
            rustls::SignatureScheme::RSA_PSS_SHA512,
            rustls::SignatureScheme::ED25519,
            rustls::SignatureScheme::ED448,
        ]
    }
}
