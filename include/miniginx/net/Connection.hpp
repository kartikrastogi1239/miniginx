#pragma once

#include "Socket.hpp"

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace miniginx::net {

// ============================================================================
// BACKGROUND: TCP stream behavior, recv(), send(), partial I/O
// ============================================================================
//
// --- TCP is a BYTE STREAM, not a message protocol -----------------------
//
// This is one of the most important and most misunderstood facts about TCP.
//
// When you call send("Hello World"), the kernel does NOT guarantee that the
// receiving side will get "Hello World" in a single recv() call. TCP is a
// continuous stream of bytes, like a pipe. The kernel is free to:
//
//   - Fragment: send("Hello World") may arrive as recv()->"Hello" then
//     recv()->" World" (split across two calls)
//   - Coalesce: two small send("Hello") + send(" World") calls may arrive
//     as a single recv()->"Hello World"
//   - Delay: data may sit in the kernel's send buffer for a few milliseconds
//     (Nagle's algorithm) before being sent, to reduce the number of tiny
//     TCP segments
//
// This means: application-level protocols (like HTTP) that sit on top of TCP
// MUST define their own framing — a way to determine where one message ends
// and the next begins. HTTP/1.1 uses blank lines and Content-Length for this.
// In this phase we just work with raw bytes and demonstrate the behavior.
//
// --- recv() -------------------------------------------------------------
//
// `ssize_t recv(int sockfd, void* buf, size_t len, int flags);`
//
// Reads UP TO `len` bytes from the connection into `buf`. Returns:
//   > 0  -> number of bytes actually read (can be LESS than `len`)
//   = 0  -> peer closed their end of the connection (EOF / graceful shutdown)
//   < 0  -> error; errno set (e.g. EAGAIN/EWOULDBLOCK if non-blocking and
//            no data available, EINTR if interrupted by a signal)
//
// KEY POINT: recv() returning N < len is NORMAL and expected. It does NOT
// mean an error occurred. It means the kernel only had N bytes available at
// that instant. A correct implementation must always be prepared to call
// recv() again to get the rest.
//
// This is what we call a "partial read." Naive code that assumes recv()
// fills the buffer completely is a classic, subtle bug that usually goes
// unnoticed in testing (because localhost TCP rarely fragments data) but
// manifests unpredictably in production.
//
// --- send() -------------------------------------------------------------
//
// `ssize_t send(int sockfd, const void* buf, size_t len, int flags);`
//
// Sends UP TO `len` bytes from `buf` into the connection. Returns:
//   > 0  -> number of bytes actually sent (can be LESS than `len`)
//   < 0  -> error; errno set
//
// KEY POINT: Just like recv(), send() can return N < len. This is a
// "partial write." It happens when:
//   - The kernel's socket send buffer is full (peer is reading too slowly)
//   - The socket is non-blocking and the buffer has limited space
//
// A correct implementation tracks how many bytes were actually consumed and
// calls send() again with the remaining bytes until all are sent (or an
// error occurs). Silently dropping the un-sent remainder is a bug.
//
// --- Blocking vs Non-Blocking I/O ---------------------------------------
//
// In BLOCKING mode (the default, which we use in this phase):
//   - recv() waits (sleeps the thread) until at least 1 byte is available,
//     or the connection closes
//   - send() waits until there is space in the kernel send buffer
//
// This simplifies the code (no EAGAIN handling) but means one thread can
// only handle one connection at a time. The epoll phase will switch to
// non-blocking and handle many connections in a single thread.
//
// --- Connection lifecycle -----------------------------------------------
//
// A TCP connection has these states from the application's perspective:
//
//   Connected -> Reading <-> Writing -> Closed
//
// The connection can close for several reasons:
//   - Peer closed gracefully: recv() returns 0
//   - Peer crashed / network error: recv() or send() returns -1
//   - We decide to close: we call close() on the socket
//   - Timeout: (future, not in this phase)
//
// Our Connection class models this with an explicit State enum and guards
// all I/O operations against attempts to use a closed connection.
// ============================================================================


// ============================================================================
// Connection: represents one active TCP client connection.
// ============================================================================
// Responsibilities:
//   - Own the client's Socket (RAII: close on destruction or explicit close).
//   - Provide safe, well-documented recv() and send() wrappers that handle
//     the most critical edge cases (partial I/O, peer-closed detection).
//   - Track the connection's lifecycle state so callers can always query
//     whether the connection is still usable.
//   - Store the client's IP for logging throughout the connection's lifetime
//     (the IP is captured at accept() time and stays valid even after close).
//
// This class does NOT:
//   - Know about HTTP, headers, or request/response structure (next phase)
//   - Manage threading or epoll registration (later phases)
//   - Implement keep-alive logic (HTTP phase)
//
// Ownership model:
//   - Non-copyable: only one Connection should own any given client socket.
//   - Movable: allows storing Connection in containers (e.g. std::vector)
//     and returning it from factory functions.
// ============================================================================
class Connection {
public:

    // ------------------------------------------------------------------
    // Connection lifecycle states.
    //
    // Modeled as an explicit enum (not bool) because "is alive?" is not
    // the only useful question — the HTTP phase will add states like
    // "reading headers", "reading body", "sending response" etc.
    // Using an enum now makes that evolution seamless.
    //
    // Connected  -> the fd is open and I/O is possible
    // Closed     -> the fd has been closed (by us or the peer);
    //               no further I/O is possible on this object
    // Error      -> a non-recoverable I/O error occurred; fd may or
    //               may not still be open but should not be used
    // ------------------------------------------------------------------
    enum class State {
        Connected,
        Closed,
        Error,
    };

    // ------------------------------------------------------------------
    // ReadResult: the outcome of a recv() attempt.
    //
    // We model this as a struct rather than returning raw ssize_t so
    // callers can use pattern-matching idioms and can't forget to check
    // "did the peer close?" vs "did I get data?" vs "error?".
    // ------------------------------------------------------------------
    struct ReadResult {
        std::vector<std::byte> data;  // bytes received (may be empty)
        bool peer_closed = false;     // true if recv() returned 0 (EOF)
        bool ok = true;               // false if a syscall error occurred
        std::string error_message;    // set when ok == false
    };

    // ------------------------------------------------------------------
    // WriteResult: the outcome of a send() attempt.
    //
    // `bytes_sent` will equal the requested amount on success (our
    // writeAll() loop guarantees this). If it's less, something went
    // wrong.
    // ------------------------------------------------------------------
    struct WriteResult {
        std::size_t bytes_sent = 0;  // total bytes actually sent
        bool ok = true;              // false if a syscall error occurred
        std::string error_message;   // set when ok == false
    };

    // ------------------------------------------------------------------
    // Constructor: takes ownership of a client fd and stores the IP.
    //
    // `socket`    - rvalue reference: we MOVE the Socket here, making
    //               Connection the new and sole owner of the fd.
    //               The caller's Socket is left in the "empty" state.
    // `peer_ip`   - client's IPv4 address string (e.g. "192.168.1.5"),
    //               captured once and stored for the lifetime of the
    //               Connection object.
    // ------------------------------------------------------------------
    Connection(Socket&& socket, std::string peer_ip);

    // Non-copyable: one fd, one owner.
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Movable: necessary for storing in std::vector, returning from
    // functions, etc. Move leaves the source Connection in Closed state.
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;

    // ------------------------------------------------------------------
    // Destructor: closes the connection if still open.
    //
    // The Socket member handles fd cleanup automatically via its own
    // destructor. We just need to update our state if desired.
    // (In practice, since the fd is closed regardless, state tracking
    // in the destructor is for completeness / future debug logging.)
    // ------------------------------------------------------------------
    ~Connection();

    // ------------------------------------------------------------------
    // read(max_bytes):
    //
    // Attempts a SINGLE recv() call reading up to `max_bytes` bytes.
    //
    // Returns a ReadResult describing what happened. Callers must inspect
    // the result - particularly `peer_closed` and `ok` - before accessing
    // `data`. See ReadResult docs above and the implementation comments
    // for the full behavior.
    //
    // This is a "single-shot" read: it does NOT loop. For higher-level
    // framing (e.g. "read until \r\n\r\n for HTTP headers"), use readUntil()
    // or implement framing above this layer.
    // ------------------------------------------------------------------
    [[nodiscard]] ReadResult read(std::size_t max_bytes = 4096);

    // ------------------------------------------------------------------
    // readUntil(delimiter, max_bytes):
    //
    // Reads bytes repeatedly until `delimiter` appears in the accumulated
    // data, or until max_bytes total have been read, or until the
    // connection closes/errors.
    //
    // Demonstrates how application-level framing is built on top of raw
    // recv() in a TCP stream. The HTTP parser in the next phase will use
    // exactly this pattern (delimiter = "\r\n\r\n" for headers).
    //
    // Returns the ReadResult of the last individual recv(), with `data`
    // containing all accumulated bytes up to and including the delimiter.
    // ------------------------------------------------------------------
    [[nodiscard]] ReadResult readUntil(
        const std::string& delimiter,
        std::size_t max_bytes = 65536);

    // ------------------------------------------------------------------
    // write(data):
    //
    // Sends ALL of `data` reliably, looping over send() until every byte
    // has been accepted by the kernel. This is "write-all" semantics.
    //
    // Why loop? Because send() can return N < data.size() (partial write)
    // even in blocking mode when the send buffer is full. A naive single
    // send() call that ignores the return value silently loses data.
    //
    // Returns a WriteResult. On success, bytes_sent == data.size().
    // On failure, bytes_sent indicates how many bytes were sent before
    // the error.
    // ------------------------------------------------------------------
    [[nodiscard]] WriteResult write(const std::vector<std::byte>& data);

    // ------------------------------------------------------------------
    // write(string): convenience overload - sends a string (e.g. a raw
    // HTTP response line or echo message) without the caller needing to
    // manually convert to bytes.
    // ------------------------------------------------------------------
    [[nodiscard]] WriteResult write(std::string_view data);

    // ------------------------------------------------------------------
    // close(): explicitly close this connection.
    //
    // After this call, state() == State::Closed and any further read()
    // or write() calls will immediately return an error result (not crash,
    // not block forever - just a clean "already closed" result).
    //
    // Safe to call multiple times.
    // ------------------------------------------------------------------
    void close() noexcept;

    // ------------------------------------------------------------------
    // Accessors / queries
    // ------------------------------------------------------------------

    [[nodiscard]] State       state()     const noexcept { return state_; }
    [[nodiscard]] bool        isAlive()   const noexcept { return state_ == State::Connected; }
    [[nodiscard]] int         fd()        const noexcept { return socket_.fd(); }
    [[nodiscard]] std::string peerIp()    const noexcept { return peer_ip_; }

private:
    Socket      socket_;    // owns the client connection's fd (RAII)
    std::string peer_ip_;   // e.g. "127.0.0.1" - set once, never changes
    State       state_;     // current lifecycle state

    // ------------------------------------------------------------------
    // Internal helper: marks the connection as errored, closes the fd,
    // and returns a ReadResult or WriteResult populated with the given
    // error message and errno info. Used to centralize error handling
    // so the public methods stay readable.
    // ------------------------------------------------------------------
    void markError() noexcept;
};

}  // namespace miniginx::net