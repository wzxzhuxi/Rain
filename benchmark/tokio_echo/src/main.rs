// Tokio TCP Echo Benchmark (对标 Rain echo_bench)
//
// 依赖:
//   cargo init rain/benchmark/tokio_echo --name tokio_echo
//   在 Cargo.toml 添加: tokio = { version = "1", features = ["full"] }
//
// 编译:
//   cd rain/benchmark/tokio_echo && cargo build --release
//
// 压测:
//   ./target/release/tokio_echo
//   wrk -t4 -c100 -d10s http://127.0.0.1:7779/

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;

const RESPONSE: &[u8] = b"HTTP/1.1 200 OK\r\n\
Content-Type: text/html; charset=utf-8\r\n\
Content-Length: 13\r\n\
Connection: keep-alive\r\n\
\r\n\
Hello, World!";

const PORT: u16 = 7779;

async fn handle_conn(mut stream: tokio::net::TcpStream) {
    let mut buf = [0u8; 128];
    loop {
        let n = match stream.read(&mut buf).await {
            Ok(0) | Err(_) => return,
            Ok(n) => n,
        };
        let _ = n;
        if stream.write_all(RESPONSE).await.is_err() {
            return;
        }
    }
}

#[tokio::main]
async fn main() {
    let listener = TcpListener::bind(format!("0.0.0.0:{PORT}"))
        .await
        .expect("bind failed");

    println!("tokio echo benchmark :{PORT}");

    loop {
        let (stream, _) = match listener.accept().await {
            Ok(conn) => conn,
            Err(_) => continue,
        };
        stream.set_nodelay(true).ok();
        tokio::spawn(handle_conn(stream));
    }
}
