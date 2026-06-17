// =============================================================================
// Mini Nginx - Phase 2: Connection Abstraction Demo
//
// This phase builds directly on the Phase 1 ServerSocket / Socket foundation
// and demonstrates the Connection class with realistic I/O.
//
// What this server does:
//   1. Listens on port 8080.
//   2. For each client connection:
//      a. Reads everything the client sends (until \r\n\r\n or EOF).
//         This mimics the first step of HTTP - without any actual HTTP
//         parsing yet, just raw byte accumulation.
//      b. Prints the raw request bytes to stdout.
//      c. Sends a plain-text "acknowledgement" response back.
//      d. Connection's destructor closes the fd when the scope ends.
//
// Key behaviors demonstrated:
//   - Connection owns the client socket (RAII): no explicit close() needed.
//   - readUntil() hides the recv() loop and partial-read handling.
//   - write() hides the send() loop and partial-write handling.
//   - ReadResult / WriteResult make error checking explicit and readable.
//   - Printing the connection state before and after shows the lifecycle.
//
// Try it:
//   $ ./miniginx
//
//   $ curl -v http://127.0.0.1:8080/hello
//   $ echo "Hello server" | nc 127.0.0.1 8080
//   $ telnet 127.0.0.1 8080
//     (type some text, then press Enter twice)
// =============================================================================

#include "miniginx/net/ServerSocket.hpp"
#include "miniginx/net/Connection.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <exception>

// ---------------------------------------------------------------------------
// handleClient()
//
// Called once per accepted client. Receives the connection by move (not copy)
// so Connection uniquely owns the client fd for the duration of this function.
// When the function returns, `conn` is destroyed and the fd is closed.
//
// Separated from main() for clarity - in later phases this becomes a method
// on a Connection or Worker class.
// ---------------------------------------------------------------------------
void handleClient(miniginx::net::Connection conn) {

    std::cout << "\n[+] Client connected: " << conn.peerIp()
              << "  (fd=" << conn.fd() << ")\n";

    // -----------------------------------------------------------------------
    // Phase 1 of reading from a TCP client: readUntil("\r\n\r\n")
    //
    // We read until the HTTP-style double-CRLF header terminator, or
    // until the client closes, or until we hit our 64KB safety cap.
    //
    // WHY "\r\n\r\n"?
    // Even though we aren't doing HTTP parsing yet, real HTTP clients
    // (curl, browsers, telnet) send requests ending with \r\n\r\n. Using
    // this delimiter means our server gracefully handles real HTTP requests
    // in this phase and appears to accept them before sending its reply.
    //
    // For a plain `nc` or `telnet` client that doesn't send the HTTP
    // terminator, we handle the peer_closed path below.
    // -----------------------------------------------------------------------
    auto result = conn.readUntil("\r\n\r\n", 65536);

    // -----------------------------------------------------------------------
    // Interpret the ReadResult and decide what to do next.
    //
    // This structured check is the "check every result" pattern that
    // MSG_NOSIGNAL + ReadResult / WriteResult structs enforce. Without these
    // structs, it's easy to skip the peer_closed check and build half-baked
    // responses.
    // -----------------------------------------------------------------------

    if (!result.ok) {
        std::cout << "[-] Read error from " << conn.peerIp()
                  << ": " << result.error_message << "\n";
        return;
    }

    if (result.peer_closed && result.data.empty()) {
        std::cout << "[-] " << conn.peerIp()
                  << " closed connection without sending data.\n";
        return;
    }

    // -----------------------------------------------------------------------
    // Print what we received (as text, for demo purposes).
    // -----------------------------------------------------------------------
    std::string_view raw_data(
        reinterpret_cast<const char*>(result.data.data()),
        result.data.size());

    std::cout << "[<] Received " << result.data.size()
              << " bytes from " << conn.peerIp() << ":\n"
              << "--- BEGIN ---\n"
              << raw_data
              << "\n--- END ---\n";

    if (result.peer_closed) {
        std::cout << "    (peer closed after sending the above)\n";
    }

    // -----------------------------------------------------------------------
    // Build and send a response.
    //
    // This is a deliberately minimal plain-text response. It has just enough
    // HTTP framing that `curl -v` won't error out: a status line, a couple of
    // headers, and a body. The HTTP parser phase will generate proper
    // responses with full header parsing.
    // -----------------------------------------------------------------------
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 38\r\n"
        "\r\n"
        "Hello from Mini Nginx (Phase 2)!\r\n"
        "\r\n";

    auto write_result = conn.write(response);

    if (!write_result.ok) {
        std::cout << "[-] Write error to " << conn.peerIp()
                  << ": " << write_result.error_message
                  << " (sent " << write_result.bytes_sent << " bytes)\n";
    } else {
        std::cout << "[>] Sent " << write_result.bytes_sent
                  << " bytes to " << conn.peerIp() << "\n";
    }

    // -----------------------------------------------------------------------
    // conn goes out of scope here.
    // ~Connection() -> Socket::~Socket() -> ::close(fd_)
    // TCP FIN sent. No explicit close() call needed anywhere.
    // -----------------------------------------------------------------------
    std::cout << "[-] Connection to " << conn.peerIp()
              << " closed (fd destroyed)\n";
}


// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------
int main() {
    constexpr uint16_t kPort = 8080;

    try {
        miniginx::net::ServerSocket server(kPort);

        std::cout << "==============================================\n"
                  << "  Mini Nginx - Phase 2: Connection Abstraction\n"
                  << "  Listening on port " << kPort << "\n"
                  << "  Try: curl http://127.0.0.1:" << kPort << "/\n"
                  << "  Try: echo 'hi' | nc 127.0.0.1 " << kPort << "\n"
                  << "==============================================\n\n";

        while (true) {
            auto [client_socket, client_ip] = server.accept();

            miniginx::net::Connection conn(
                std::move(client_socket),
                client_ip);

            handleClient(std::move(conn));
        }

    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}