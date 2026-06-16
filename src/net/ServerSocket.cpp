#include "./../include/miniginx/net/ServerSocket.hpp"

#include <stdexcept>
#include <cstring>   // std::memset, std::strerror
#include <cerrno>    // errno

namespace miniginx::net {

namespace {

// ----------------------------------------------------------------------
// Small helper to turn a failed syscall's errno into a descriptive
// exception. Centralizing this avoids repeating the same
// "strerror(errno)" boilerplate after every syscall below.
// ----------------------------------------------------------------------
[[noreturn]] void throwErrno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

}  // namespace


// ============================================================================
// ServerSocket constructor
//
// Walks through the full "become a listening server socket" sequence.
// Each step is commented with WHAT it does and WHY it's needed.
// ============================================================================
ServerSocket::ServerSocket(uint16_t port, int backlog) {

    // ------------------------------------------------------------------
    // STEP 1: socket() - create the socket and obtain its fd.
    //
    //   AF_INET     -> IPv4
    //   SOCK_STREAM -> TCP (reliable, ordered, connection-oriented)
    //   protocol=0  -> let the kernel choose (TCP, for this combination)
    //
    // On success, returns a non-negative fd. On failure, returns -1 and
    // sets errno.
    // ------------------------------------------------------------------
    int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (raw_fd < 0) {
        throwErrno("socket() failed");
    }

    // Immediately wrap the raw fd in our RAII Socket. From this point on,
    // even if a later step throws, the destructor of `listen_socket_`
    // (which runs during stack unwinding) will close this fd for us.
    // No manual cleanup / goto-style error handling needed.
    listen_socket_ = Socket(raw_fd);

    // ------------------------------------------------------------------
    // STEP 2: setsockopt(SO_REUSEADDR) - allow immediate port reuse.
    //
    // WHY THIS MATTERS:
    // When a TCP server socket is closed, the OS may keep the port in a
    // TIME_WAIT state for a couple of minutes (this is part of TCP's
    // protocol design, to handle stray packets from the old connection).
    // Without SO_REUSEADDR, restarting the server quickly (e.g. during
    // development, after Ctrl+C) would fail with "Address already in
    // use" (EADDRINUSE) even though the old process is gone.
    //
    // SO_REUSEADDR tells the kernel: "it's fine to bind to this
    // address/port even if a previous socket using it is still in
    // TIME_WAIT." This is standard practice for server sockets and is
    // essentially always set in real server code.
    // ------------------------------------------------------------------
    int reuse = 1;
    if (::setsockopt(listen_socket_.fd(), SOL_SOCKET, SO_REUSEADDR,
                      &reuse, sizeof(reuse)) < 0) {
        throwErrno("setsockopt(SO_REUSEADDR) failed");
    }

    // ------------------------------------------------------------------
    // STEP 3: Build the sockaddr_in describing "0.0.0.0:port".
    //
    // sockaddr_in layout (see header comment for the full struct):
    //   sin_family -> AF_INET (must match the family used in socket())
    //   sin_port   -> the TCP port, in NETWORK byte order
    //   sin_addr   -> the IP address to bind to, in NETWORK byte order
    //
    // We zero-initialize the whole struct first. This is important
    // because sockaddr_in on Linux has reserved/padding bytes that must
    // be zero - leaving them uninitialized is undefined behavior and can
    // cause bind() to behave unpredictably on some systems.
    // ------------------------------------------------------------------
    sockaddr_in addr{};
    std::memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;

    // htons = "host to network short": converts a 16-bit port number
    // from the host machine's native byte order (which on x86/ARM is
    // little-endian) to network byte order (big-endian), as required by
    // the sockaddr_in ABI.
    addr.sin_port = htons(port);

    // INADDR_ANY (0.0.0.0) means "bind to all available local network
    // interfaces" - so the server accepts connections via loopback
    // (127.0.0.1), the machine's LAN IP, etc. INADDR_ANY is already
    // defined in the correct (network) byte order as 0, so no htonl()
    // is needed here, but for a *specific* IP you would use
    // inet_pton()/htonl().
    addr.sin_addr.s_addr = INADDR_ANY;

    // ------------------------------------------------------------------
    // STEP 4: bind() - associate the socket with 0.0.0.0:port.
    //
    // We pass `&addr` as `const sockaddr*` (it's actually a
    // `sockaddr_in*` underneath - this reinterpret_cast is the standard,
    // expected pattern in POSIX socket code, since the syscall interface
    // is generic over address families). We also pass sizeof(addr) so
    // the kernel knows how many bytes to read from that pointer for this
    // particular address family.
    //
    // After this call succeeds, the kernel knows: "TCP segments destined
    // for port `port` on any local interface belong to this socket."
    // ------------------------------------------------------------------
    if (::bind(listen_socket_.fd(),
                reinterpret_cast<const sockaddr*>(&addr),
                sizeof(addr)) < 0) {
        throwErrno("bind() failed");
    }

    // ------------------------------------------------------------------
    // STEP 5: listen() - mark the socket as passive/listening.
    //
    // `backlog` bounds how many fully-established (post-handshake)
    // connections can sit in the kernel's accept queue waiting for us to
    // call accept(). If a burst of clients connects faster than we
    // accept() them, up to `backlog` connections will queue; beyond that,
    // the kernel may refuse further attempts.
    //
    // After this call, the socket can start completing TCP handshakes
    // with connecting clients - even before we call accept() for the
    // first time.
    // ------------------------------------------------------------------
    if (::listen(listen_socket_.fd(), backlog) < 0) {
        throwErrno("listen() failed");
    }

    // If we reach here, listen_socket_ owns a fully bound + listening fd.
    // The object is now in a valid, ready-to-use state - this is the
    // class invariant that the rest of ServerSocket's methods can rely
    // on without re-checking.
}


// ============================================================================
// ServerSocket::accept()
//
// Accepts the next pending client connection (blocking) and returns it
// together with the client's IP address.
// ============================================================================
std::pair<Socket, std::string> ServerSocket::accept() const {

    // ------------------------------------------------------------------
    // sockaddr_in to be filled in BY THE KERNEL with the CLIENT's
    // address/port (as opposed to the constructor's sockaddr_in, which
    // we filled in OURSELVES to describe OUR OWN bind address).
    //
    // Zero-initialized for the same "no undefined padding" reason as
    // before.
    // ------------------------------------------------------------------
    sockaddr_in client_addr{};
    std::memset(&client_addr, 0, sizeof(client_addr));

    // accept() needs to know the size of the buffer it's allowed to
    // write into. It's passed by pointer because accept() also WRITES
    // BACK the actual number of bytes it used (relevant for address
    // families with variable-length addresses; for sockaddr_in it will
    // always end up equal to sizeof(sockaddr_in), but we still must
    // provide it correctly).
    socklen_t client_addr_len = sizeof(client_addr);

    // ------------------------------------------------------------------
    // accept() - the core syscall of this whole class.
    //
    // Behavior (BLOCKING mode, which is the default and what we have
    // here since we have not set O_NONBLOCK on listen_socket_):
    //   - If a completed connection is already waiting in the backlog
    //     queue, returns immediately with a new fd for it.
    //   - Otherwise, the calling thread is PUT TO SLEEP by the kernel
    //     until a client connects, at which point it wakes up and
    //     returns the new fd.
    //
    // The returned fd is a COMPLETELY SEPARATE socket from
    // listen_socket_.fd():
    //   - listen_socket_.fd() keeps listening for MORE clients
    //   - the returned fd is used to read()/write() data to/from THIS
    //     specific client, and must be close()'d when we're done with
    //     this client (handled automatically by wrapping it in Socket).
    //
    // On error, returns -1 and sets errno.
    // ------------------------------------------------------------------
    int client_fd = ::accept(
        listen_socket_.fd(),
        reinterpret_cast<sockaddr*>(&client_addr),
        &client_addr_len);

    if (client_fd < 0) {
        throwErrno("accept() failed");
    }

    // ------------------------------------------------------------------
    // Convert the client's binary IPv4 address (client_addr.sin_addr,
    // a 32-bit value in network byte order) into a human-readable
    // dotted-decimal string like "192.168.1.42".
    //
    // inet_ntoa ("network to ASCII") is the classic, simple way to do
    // this for IPv4. (The modern, IPv6-capable replacement is
    // inet_ntop(), but inet_ntoa is perfectly fine and very readable for
    // this educational IPv4-only phase.)
    //
    // NOTE: inet_ntoa returns a pointer to a statically-allocated buffer
    // that may be overwritten by subsequent calls - so we immediately
    // copy it into a std::string to own our own copy of the data before
    // returning.
    // ------------------------------------------------------------------
    std::string client_ip = ::inet_ntoa(client_addr.sin_addr);

    // Wrap the new client fd in a Socket for RAII ownership, and return
    // it together with the IP string. Returning by value here triggers
    // Socket's move constructor (or, with guaranteed copy elision in
    // C++17/20, may construct it directly in the caller's storage) -
    // either way, no double-ownership or leak occurs.
    return { Socket(client_fd), client_ip };
}

}  // namespace miniginx::net