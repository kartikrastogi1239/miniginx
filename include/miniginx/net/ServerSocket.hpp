#pragma once

#include "Socket.hpp"

#include <string>
#include <cstdint>

namespace miniginx::net {

// ============================================================================
// BACKGROUND: socket(), sockaddr, sockaddr_in, bind(), listen(), accept()
// ============================================================================
//
// --- socket() -----------------------------------------------------------
// `int socket(int domain, int type, int protocol);`
//
// This is the syscall that CREATES a new socket and returns its fd.
// Think of it as: "kernel, please allocate a new network communication
// endpoint and hand me a handle to it."
//
//   - domain   = AF_INET   -> use IPv4 addressing
//   - type     = SOCK_STREAM -> a reliable, ordered, byte-stream protocol
//                (i.e. TCP, as opposed to SOCK_DGRAM which is UDP)
//   - protocol = 0 -> let the kernel pick the default protocol for the
//                given domain/type combo (for AF_INET + SOCK_STREAM,
//                that default is TCP)
//
// At this point the socket exists but is NOT yet associated with any
// address or port, and is not listening for anything. It's just an
// anonymous endpoint sitting in the kernel.
//
// --- sockaddr / sockaddr_in ----------------------------------------------
// The kernel's socket API is protocol-agnostic at the syscall signature
// level: functions like bind() and accept() take a generic
// `struct sockaddr*` pointer plus a length. But "address" means different
// things for different protocol families (IPv4 vs IPv6 vs Unix domain
// sockets, etc.), so there's a FAMILY of structs that all start with the
// same layout as `sockaddr` but add family-specific fields after it:
//
//   struct sockaddr {
//       sa_family_t sa_family;   // address family, e.g. AF_INET
//       char        sa_data[14]; // protocol-specific address data
//   };
//
//   struct sockaddr_in {           // IPv4-specific version
//       sa_family_t    sin_family; // AF_INET
//       in_port_t      sin_port;   // port number (NETWORK byte order!)
//       struct in_addr sin_addr;   // IPv4 address (NETWORK byte order!)
//       // ... padding to match sizeof(sockaddr) ...
//   };
//
// We fill in a `sockaddr_in` (which we understand: IPv4, port, address),
// then `reinterpret_cast` its address to `sockaddr*` when calling bind()
// or accept(). This is a deliberate, historically-justified type-punning
// pattern baked into the POSIX sockets API - not something unique to this
// codebase.
//
// IMPORTANT: "network byte order" means big-endian. Multi-byte fields like
// sin_port and sin_addr must be converted from the host's native byte
// order using functions like htons() ("host to network short") and
// htonl()/inet_pton() for addresses. Forgetting this is a classic bug that
// causes a server to silently bind to the wrong port.
//
// --- bind() ---------------------------------------------------------------
// `int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);`
//
// Associates the socket with a specific local address and port. Before
// bind(), the socket has no address; after bind(), the OS knows: "any
// packet arriving on port 8080 (for this IP) should be routed to this
// socket."
//
// Binding to INADDR_ANY (0.0.0.0) means "accept connections arriving on
// ANY of this machine's network interfaces" (loopback, ethernet, etc.),
// rather than just one specific IP.
//
// --- listen() ---------------------------------------------------------------
// `int listen(int sockfd, int backlog);`
//
// Transitions the socket from "I have an address" to "I am willing to
// accept incoming connections." This is what makes the socket a
// "listening socket" / "server socket" / "passive socket."
//
// The `backlog` argument bounds the queue of connections that have
// completed the TCP handshake but haven't yet been accept()'ed by the
// application. If this queue fills up (because the app is slow to call
// accept()), new connection attempts may be refused or dropped depending
// on OS configuration.
//
// --- accept() ---------------------------------------------------------------
// `int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen);`
//
// Pulls the next completed connection off the listening socket's queue
// and returns a BRAND NEW fd representing that specific client
// connection. The original listening fd is UNCHANGED and keeps listening
// for further connections.
//
// This is a critical distinction:
//   - the LISTENING socket fd  -> never used for data transfer, only for
//                                  accept()
//   - the CONNECTION socket fd -> one per client, used for read()/write(),
//                                  and closed when that client disconnects
//
// accept() also optionally fills in a sockaddr_in with the CLIENT's
// address/port, which is how we can print "client connected from
// 127.0.0.1" etc.
//
// In this educational phase, accept() is called in BLOCKING mode: the
// call simply pauses (blocks) the calling thread until a client connects.
// That's fine for a single-connection-at-a-time demo; the upcoming epoll
// phase will replace this blocking behavior with non-blocking, event-driven
// I/O.
// ============================================================================


// ============================================================================
// ServerSocket: a Socket specialized for the "listening socket" role.
// ============================================================================
// This class encapsulates the full setup sequence:
//
//     socket()  ->  setsockopt(SO_REUSEADDR)  ->  bind()  ->  listen()
//
// and exposes a single accept() operation that returns:
//   - a new Socket owning the client connection's fd
//   - the client's IP address as a human-readable string
//
// Design notes:
//   - ServerSocket OWNS a Socket (composition, not inheritance). We use
//     composition here because a "server socket" and a "client connection
//     socket" have very different APIs/responsibilities even though they
//     both wrap an fd - inheritance would force a shared interface that
//     doesn't really make sense (a listening socket has no read()/write(),
//     a connection socket has no accept()).
//   - All constructor work that can fail throws std::runtime_error. This
//     means: if a ServerSocket object exists, it is GUARANTEED to be fully
//     bound and listening. There is no "half-constructed" state to check
//     for - this is the "RAII = valid invariants" philosophy.
// ============================================================================
class ServerSocket {
public:
    // ------------------------------------------------------------------
    // Constructs, binds, and starts listening on the given TCP port,
    // on all local interfaces (0.0.0.0).
    //
    // Throws std::runtime_error if any step (socket/setsockopt/bind/listen)
    // fails.
    //
    // `backlog` is the pending-connection queue size passed to listen().
    // ------------------------------------------------------------------
    explicit ServerSocket(uint16_t port, int backlog = 128);

    // Non-copyable (it owns a Socket, which is non-copyable).
    ServerSocket(const ServerSocket&) = delete;
    ServerSocket& operator=(const ServerSocket&) = delete;

    // Movable - ownership of the underlying listening fd can transfer.
    ServerSocket(ServerSocket&&) noexcept = default;
    ServerSocket& operator=(ServerSocket&&) noexcept = default;

    ~ServerSocket() = default;  // Socket member's destructor closes the fd

    // ------------------------------------------------------------------
    // accept(): block until a client connects, then return:
    //   - first:  a Socket owning the new client connection's fd
    //   - second: the client's IP address as a dotted-decimal string
    //             (e.g. "127.0.0.1")
    //
    // Throws std::runtime_error if the underlying accept() syscall fails.
    //
    // NOTE: This call BLOCKS the calling thread until a connection
    // arrives. In this phase that is intentional and expected - there is
    // only one thread, and its whole job is to sit here waiting for
    // clients. The epoll-based phase will change this.
    // ------------------------------------------------------------------
    [[nodiscard]] std::pair<Socket, std::string> accept() const;

    // ------------------------------------------------------------------
    // fd(): access to the underlying listening fd, e.g. for diagnostics
    // or for later registration with epoll.
    // ------------------------------------------------------------------
    [[nodiscard]] int fd() const noexcept { return listen_socket_.fd(); }

private:
    Socket listen_socket_;  // the bound, listening fd (owned via RAII)
};

}  // namespace miniginx::net