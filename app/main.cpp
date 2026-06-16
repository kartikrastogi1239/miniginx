// =============================================================================
// Mini Nginx - Phase 1: Networking Foundation Demo
//
// This program demonstrates the bare-bones TCP server lifecycle using the
// RAII Socket / ServerSocket classes from miniginx::net.
//
// What it does:
//   1. Creates a listening socket on port 8080 (socket + bind + listen,
//      all handled inside ServerSocket's constructor).
//   2. Repeatedly:
//        - Blocks in accept() until a client connects.
//        - Prints the client's IP address.
//        - Immediately closes that connection (no HTTP, no data exchange
//          yet - that comes in later phases).
//
// No epoll, no threads, no HTTP parsing. Purely: can we accept TCP
// connections safely and cleanly using RAII?
//
// Try it out:
//   $ ./miniginx
//   Mini Nginx listening on port 8080 (Ctrl+C to stop)...
//
//   # in another terminal:
//   $ curl http://127.0.0.1:8080/
//   (curl will see the connection close immediately with no response -
//    that's expected at this phase)
//
//   $ telnet 127.0.0.1 8080
//   (connects, then is immediately disconnected by the server)
// =============================================================================

#include "miniginx/net/ServerSocket.hpp"

#include <iostream>
#include <exception>

int main() {
    constexpr uint16_t kPort = 8080;

    try {
        // ------------------------------------------------------------
        // Construct the listening socket.
        //
        // By the time this constructor returns successfully, the
        // server is fully bound to 0.0.0.0:8080 and listening - i.e.
        // the OS will already accept TCP handshakes on this port, even
        // before we call accept() for the first time.
        //
        // If construction fails (port in use, permission denied for
        // privileged ports, etc.), it throws std::runtime_error, which
        // we catch below and report.
        // ------------------------------------------------------------
        miniginx::net::ServerSocket server(kPort);

        std::cout << "Mini Nginx listening on port " << kPort
                  << " (Ctrl+C to stop)...\n";

        // ------------------------------------------------------------
        // Main accept loop.
        //
        // Each iteration:
        //   - server.accept() BLOCKS until a client connects, then
        //     returns:
        //       * a Socket owning the new connection's fd
        //       * the client's IP address as a string
        //
        //   - We print the IP.
        //
        //   - `client` (the Socket) goes out of scope at the end of
        //     the loop body, so its destructor runs immediately -
        //     calling close() on the connection's fd. This is the
        //     "immediately close the connection" behavior requested
        //     for this phase, achieved purely through RAII: there is
        //     no explicit close() call anywhere in this function.
        //
        // The listening socket itself (`server`) is NEVER closed by
        // this loop - it keeps accepting new clients indefinitely
        // until the program exits, at which point ITS destructor
        // closes the listening fd too.
        // ------------------------------------------------------------
        while (true) {
            auto [client, client_ip] = server.accept();

            std::cout << "Accepted connection from " << client_ip << "\n";

            // `client` is destroyed here (end of loop body scope),
            // which closes its fd via Socket's destructor.
            std::cout << "Closing connection from " << client_ip << "\n";
        }

    } catch (const std::exception& ex) {
        // Any failure in ServerSocket's constructor (socket/setsockopt/
        // bind/listen) or in accept() lands here. We report it and exit
        // with a non-zero status.
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}