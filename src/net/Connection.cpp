#include "./../include/miniginx/net/Connection.hpp"

#include <stdexcept>
#include <cstring>      // std::strerror, std::memcpy
#include <cerrno>       // errno
#include <sys/socket.h> // recv(), send(), MSG_NOSIGNAL

namespace miniginx::net {

// ============================================================================
// A note on MSG_NOSIGNAL (used in send() below):
//
// By default on Linux, writing to a socket whose peer has closed their end
// causes the kernel to send a SIGPIPE signal to the process. The DEFAULT
// handler for SIGPIPE kills the process silently. This is almost never what
// a server wants.
//
// Solutions:
//   1. signal(SIGPIPE, SIG_IGN) in main() - ignore SIGPIPE globally
//   2. Pass MSG_NOSIGNAL flag to each send() call - suppress the signal
//      just for that call, and return -1/EPIPE instead
//
// We use option 2 (MSG_NOSIGNAL) because it's explicit, local to the
// send call, and doesn't affect other signal handling in the program.
// The send() will return -1 with errno = EPIPE if the peer has gone away,
// which we handle as a normal "peer closed" error.
// ============================================================================


// ----------------------------------------------------------------------------
// Constructor
// ----------------------------------------------------------------------------
Connection::Connection(Socket&& socket, std::string peer_ip)
    : socket_(std::move(socket))    // take ownership of the client fd
    , peer_ip_(std::move(peer_ip))  // copy the IP string once
    , state_(State::Connected)      // start in the Connected state
{
    // Invariant: if we constructed successfully, we own a valid fd and
    // are ready for I/O. No further setup needed in this phase.
}


// ----------------------------------------------------------------------------
// Move constructor: transfer ownership, leave source in Closed state.
// ----------------------------------------------------------------------------
Connection::Connection(Connection&& other) noexcept
    : socket_(std::move(other.socket_))
    , peer_ip_(std::move(other.peer_ip_))
    , state_(other.state_)
{
    other.state_ = State::Closed;  // source no longer owns anything
}


// ----------------------------------------------------------------------------
// Move assignment: same as move ctor, but we must close ourselves first.
// ----------------------------------------------------------------------------
Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();  // release our current fd (if any) before overwriting
        socket_   = std::move(other.socket_);
        peer_ip_  = std::move(other.peer_ip_);
        state_    = other.state_;
        other.state_ = State::Closed;
    }
    return *this;
}


// ----------------------------------------------------------------------------
// Destructor: the Socket member closes the fd automatically. We just make
// sure our state reflects that we're gone.
// ----------------------------------------------------------------------------
Connection::~Connection() {
    // Socket's destructor will call close(fd_) if the fd is still valid.
    // Nothing else to do here for Phase 2. Later phases may log "connection
    // to X destroyed after N bytes" etc.
    state_ = State::Closed;
}


// ----------------------------------------------------------------------------
// close(): explicitly end this connection.
// ----------------------------------------------------------------------------
void Connection::close() noexcept {
    if (state_ == State::Connected) {
        socket_.close();       // sends TCP FIN to the peer
        state_ = State::Closed;
    }
}


// ----------------------------------------------------------------------------
// markError(): internal helper called when a syscall fails.
//
// Closes the fd (no point keeping it open after an error on a TCP stream)
// and updates state so future I/O calls bail out immediately without
// making further syscalls.
// ----------------------------------------------------------------------------
void Connection::markError() noexcept {
    socket_.close();
    state_ = State::Error;
}


// ============================================================================
// read() - single recv() attempt
// ============================================================================
//
// FULL EXPLANATION OF THE recv() SEMANTICS DEMONSTRATED HERE:
//
//   recv() is the fundamental "read bytes from a socket" syscall. It is
//   analogous to read() but is socket-specific and supports flags.
//
//   Signature:
//       ssize_t recv(int sockfd, void* buf, size_t len, int flags);
//
//   Return value interpretation (the most important part to get right):
//
//     n > 0   Data received. `n` bytes are now in `buf`. Critically:
//             n MAY BE LESS THAN `len`. This is a PARTIAL READ. The
//             remaining bytes may arrive in the next recv() call, or
//             they may be bundled with bytes from the next send(). The
//             caller MUST handle this. We do so via readUntil() for
//             framed data, or by returning n to the caller for raw reads.
//
//     n = 0   EOF / graceful close. The peer has called close() or
//             shutdown(SHUT_WR) on their end. No more data will ever
//             arrive. The connection should be considered closed. Any
//             pending unsent data we have for the peer should be
//             discarded (or flushed first if needed).
//
//     n < 0   Error. errno tells us what went wrong:
//               EINTR        - interrupted by a signal before any bytes
//                              were read; it's safe to retry immediately
//               EAGAIN /     - no data available right now (only meaningful
//               EWOULDBLOCK    for non-blocking sockets; won't happen in
//                              blocking mode except after SO_RCVTIMEO)
//               ECONNRESET   - peer sent TCP RST (abnormal close, e.g. crash)
//               others       - genuine errors, close the connection
//
// FLAGS:
//   We pass 0 (no special flags). Other useful flags include:
//     MSG_PEEK  - read data without removing it from the buffer
//     MSG_WAITALL - try harder to fill the buffer (still not guaranteed!)
//     MSG_DONTWAIT - equivalent to O_NONBLOCK for a single call
// ============================================================================
Connection::ReadResult Connection::read(std::size_t max_bytes) {

    // Guard: don't attempt I/O on a closed/errored connection.
    if (state_ != State::Connected) {
        return ReadResult{
            .data          = {},
            .peer_closed   = (state_ == State::Closed),
            .ok            = false,
            .error_message = "Connection is not in Connected state"
        };
    }

    // Allocate a receive buffer on the stack (for small reads) or heap.
    // 4096 bytes is a common choice because it matches one memory page,
    // which often aligns with how the kernel delivers data to us.
    // The caller can override max_bytes for larger or smaller reads.
    std::vector<std::byte> buf(max_bytes);

    // ------------------------------------------------------------------
    // The actual recv() call.
    //
    // `static_cast<void*>(buf.data())` - recv wants a void*, std::byte*
    // is not implicitly convertible, so we cast explicitly.
    //
    // `buf.size()` - maximum bytes to receive in this single call.
    //
    // `0` - no special flags (blocking, no MSG_PEEK, etc.)
    // ------------------------------------------------------------------
    ssize_t n = ::recv(socket_.fd(),
                       static_cast<void*>(buf.data()),
                       buf.size(),
                       0);

    if (n > 0) {
        // ---------------------------------------------------------------
        // PARTIAL READ case: n may be less than buf.size().
        // We MUST resize the vector to the actual number of bytes received,
        // otherwise buf.size() == max_bytes but only the first n bytes
        // contain valid data. The rest would be zeros (from value-init).
        //
        // This resize is "free" (doesn't reallocate) when shrinking.
        // ---------------------------------------------------------------
        buf.resize(static_cast<std::size_t>(n));
        return ReadResult{
            .data          = std::move(buf),
            .peer_closed   = false,
            .ok            = true,
            .error_message = {}
        };
    }

    if (n == 0) {
        // ---------------------------------------------------------------
        // EOF: peer closed their write end of the connection.
        //
        // This is a NORMAL, non-error event. We update our state to
        // Closed and close our socket too (no point keeping a half-open
        // connection in this phase; keep-alive logic comes with HTTP).
        // ---------------------------------------------------------------
        close();
        return ReadResult{
            .data          = {},
            .peer_closed   = true,
            .ok            = true,
            .error_message = {}
        };
    }

    // n < 0: a syscall error occurred.
    //
    // Special case: EINTR means a signal interrupted the syscall before
    // it could read anything. The socket is still healthy; the caller
    // can retry. We return ok=false with a specific message so the
    // caller can decide whether to retry.
    if (errno == EINTR) {
        return ReadResult{
            .data          = {},
            .peer_closed   = false,
            .ok            = false,
            .error_message = "recv() interrupted by signal (EINTR); retry"
        };
    }

    // Any other error is treated as fatal for this connection.
    std::string errmsg = "recv() failed: " + std::string(std::strerror(errno));
    markError();
    return ReadResult{
        .data          = {},
        .peer_closed   = false,
        .ok            = false,
        .error_message = std::move(errmsg)
    };
}


// ============================================================================
// readUntil() - loop recv() until a delimiter sequence appears
// ============================================================================
//
// This method demonstrates the fundamental pattern for ALL framed protocols
// on top of TCP:
//
//   "Keep accumulating raw bytes until you see the application-level
//    boundary marker."
//
// HTTP/1.1 uses "\r\n\r\n" to end the header section. A custom protocol
// might use "\n", "\0", a fixed-length length prefix, etc.
//
// The loop here is:
//   1. recv() some bytes
//   2. Append to accumulation buffer
//   3. Search for delimiter in the whole accumulated buffer
//   4. If found, done. If not, go to 1.
//   5. If connection closes or errors, return whatever we have so far.
//   6. If we hit max_bytes without finding the delimiter, return an error
//      (protects against unbounded memory growth from a misbehaving client).
//
// Important: the delimiter might arrive SPLIT ACROSS TWO recv() CALLS.
//   e.g. first recv() gives "...HEAD\r\n" and second gives "\r\nBody..."
//   That's why we search the WHOLE accumulated buffer after each recv(),
//   not just the newly-arrived chunk.
// ============================================================================
Connection::ReadResult Connection::readUntil(
    const std::string& delimiter,
    std::size_t max_bytes)
{
    if (state_ != State::Connected) {
        return ReadResult{
            .data          = {},
            .peer_closed   = (state_ == State::Closed),
            .ok            = false,
            .error_message = "Connection is not in Connected state"
        };
    }

    std::vector<std::byte> accumulated;
    accumulated.reserve(4096);  // reasonable initial allocation

    while (accumulated.size() < max_bytes) {

        // Read a chunk (at most what's left of our max budget).
        std::size_t remaining = max_bytes - accumulated.size();
        auto chunk_result = read(std::min(remaining, std::size_t{4096}));

        if (!chunk_result.ok) {
            // I/O error: return whatever we managed to accumulate plus
            // the error info from the chunk read.
            chunk_result.data = std::move(accumulated);
            return chunk_result;
        }

        if (chunk_result.peer_closed) {
            // Peer closed before we found the delimiter. Return what
            // we have; callers can decide if a truncated message is usable.
            return ReadResult{
                .data          = std::move(accumulated),
                .peer_closed   = true,
                .ok            = true,
                .error_message = {}
            };
        }

        // Append the new chunk to our accumulation buffer.
        accumulated.insert(
            accumulated.end(),
            chunk_result.data.begin(),
            chunk_result.data.end());

        // ---------------------------------------------------------------
        // Search for the delimiter in the FULL accumulated buffer.
        //
        // We convert to string_view to use std::string::find(), since
        // std::byte is not a char. In a production implementation you
        // would do this search directly over the byte buffer for
        // efficiency; here clarity is prioritized.
        // ---------------------------------------------------------------
        std::string_view view(
            reinterpret_cast<const char*>(accumulated.data()),
            accumulated.size());

        auto pos = view.find(delimiter);
        if (pos != std::string_view::npos) {
            // Found the delimiter. Truncate accumulated to include the
            // delimiter itself (callers typically want it for parsing).
            accumulated.resize(pos + delimiter.size());
            return ReadResult{
                .data          = std::move(accumulated),
                .peer_closed   = false,
                .ok            = true,
                .error_message = {}
            };
        }
    }

    // Reached max_bytes without finding the delimiter.
    return ReadResult{
        .data          = std::move(accumulated),
        .peer_closed   = false,
        .ok            = false,
        .error_message = "readUntil: max_bytes reached without finding delimiter"
    };
}


// ============================================================================
// write(bytes) - send ALL bytes, looping over send() to handle partial writes
// ============================================================================
//
// FULL EXPLANATION OF THE send() SEMANTICS DEMONSTRATED HERE:
//
//   send() is the fundamental "write bytes to a socket" syscall.
//
//   Signature:
//       ssize_t send(int sockfd, const void* buf, size_t len, int flags);
//
//   Return value:
//     n > 0   `n` bytes were accepted into the kernel's SEND BUFFER.
//             This does NOT mean the peer received them yet - it means
//             they are now the kernel's responsibility to deliver.
//             CRITICALLY: n may be LESS than len (partial write).
//
//     n < 0   Error. Important errno values:
//               EINTR      - interrupted by signal; retry is safe
//               EAGAIN /   - send buffer full; only for non-blocking sockets
//               EWOULDBLOCK
//               EPIPE      - peer closed their end; we got a RST back.
//                            Without MSG_NOSIGNAL this would also raise SIGPIPE
//               ECONNRESET - connection reset by peer
//
// WHY PARTIAL WRITES HAPPEN (even in blocking mode):
//
//   The kernel has a send buffer per socket (typically 128KB-4MB, tunable
//   via SO_SNDBUF). send() copies bytes from your buffer into the kernel's
//   buffer. If the kernel buffer is nearly full (because the peer is reading
//   slowly), send() in blocking mode will BLOCK until there's space for at
//   least 1 byte, then return however many bytes it accepted.
//
//   So if you try to send 1MB at once and the kernel buffer only has 256KB
//   free, send() returns 262144. Your code MUST then call send() again with
//   the remaining 786432 bytes. Repeat until done or error.
//
//   Our write() method does exactly this loop. The caller never needs to
//   worry about partial sends - they just pass all their data and either
//   get "all sent" or "error at byte N."
//
// FLAGS:
//   MSG_NOSIGNAL - suppresses SIGPIPE if the peer has closed their end.
//                  Without this, a closed-peer write would kill the process.
//                  With this, send() returns -1 / errno=EPIPE instead.
// ============================================================================
Connection::WriteResult Connection::write(const std::vector<std::byte>& data) {

    if (state_ != State::Connected) {
        return WriteResult{
            .bytes_sent    = 0,
            .ok            = false,
            .error_message = "Connection is not in Connected state"
        };
    }

    if (data.empty()) {
        return WriteResult{ .bytes_sent = 0, .ok = true, .error_message = {} };
    }

    std::size_t total_sent = 0;
    const std::byte* ptr   = data.data();
    std::size_t remaining  = data.size();

    // ------------------------------------------------------------------
    // THE WRITE-ALL LOOP:
    //
    // We keep calling send() until all bytes have been accepted by the
    // kernel. Each iteration:
    //   - sends starting from `ptr` (which advances by what was sent)
    //   - reduces `remaining` by what was sent
    //   - stops when remaining == 0 (all done) or error occurs
    // ------------------------------------------------------------------
    while (remaining > 0) {

        ssize_t n = ::send(
            socket_.fd(),
            static_cast<const void*>(ptr),
            remaining,
            MSG_NOSIGNAL   // don't raise SIGPIPE if peer closed their end
        );

        if (n > 0) {
            // n bytes sent. Advance our position in the buffer.
            std::size_t sent = static_cast<std::size_t>(n);
            total_sent += sent;
            ptr        += sent;
            remaining  -= sent;
            // Continue loop - may still have bytes to send.
            continue;
        }

        // n == 0 should not happen for send() (it always sends >= 1 byte
        // or returns an error), but we guard for it defensively.
        if (n == 0) {
            break;
        }

        // n < 0: error.
        if (errno == EINTR) {
            // Signal interrupted us before any bytes were sent this
            // iteration. The socket is still healthy; retry immediately.
            continue;
        }

        // EPIPE or ECONNRESET means the peer has gone away. Not a
        // programming error on our side, just the end of the connection.
        if (errno == EPIPE || errno == ECONNRESET) {
            markError();
            return WriteResult{
                .bytes_sent    = total_sent,
                .ok            = false,
                .error_message = "send() failed: peer closed connection ("
                                 + std::string(std::strerror(errno)) + ")"
            };
        }

        // Any other error is also fatal for this connection.
        std::string errmsg = "send() failed: " + std::string(std::strerror(errno));
        markError();
        return WriteResult{
            .bytes_sent    = total_sent,
            .ok            = false,
            .error_message = std::move(errmsg)
        };
    }

    return WriteResult{
        .bytes_sent    = total_sent,
        .ok            = true,
        .error_message = {}
    };
}


// ============================================================================
// write(string_view) - convenience overload
// ============================================================================
// Converts a string (e.g. a literal "Hello, world!\n" or a built-up HTTP
// response string) to bytes and delegates to the vector overload.
//
// This is a thin adapter: all the real logic lives in write(vector<byte>).
// We do NOT duplicate the send() loop here.
// ============================================================================
Connection::WriteResult Connection::write(std::string_view data) {

    // Reinterpret the char* as std::byte* - this reinterpret_cast is
    // explicitly permitted by the C++ standard for std::byte (which is
    // designed as a "bag of bits" type).
    const std::byte* begin = reinterpret_cast<const std::byte*>(data.data());
    const std::byte* end   = begin + data.size();

    return write(std::vector<std::byte>(begin, end));
}

}  // namespace miniginx::net