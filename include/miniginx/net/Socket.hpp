#pragma once

#include <string>
#include <stdexcept>
#include <utility>

#include <unistd.h>      // close(), read(), write()
#include <sys/socket.h>  // socket(), bind(), listen(), accept(), sockaddr
#include <netinet/in.h>  // sockaddr_in, htons, INADDR_ANY
#include <arpa/inet.h>   // inet_ntoa, inet_ntop

namespace miniginx::net {

// ============================================================================
// BACKGROUND: What is a "file descriptor"?
// ============================================================================
// On Linux, (almost) everything is represented as a "file descriptor" (fd) -
// a small non-negative integer that the kernel uses as a handle/index into
// its internal table of open resources for a process. Files, pipes, and
// network sockets are ALL just file descriptors from the perspective of
// user-space code.
//
// When we call socket(), the kernel:
//   1. Allocates an internal data structure representing a network endpoint
//      (protocol state, buffers, etc.)
//   2. Returns an integer (the fd) that we use to refer to that structure
//      in all future syscalls (bind, listen, accept, read, write, close).
//
// fd's are a SCARCE, OS-level resource: if we forget to close() one, it
// leaks until the process exits (or until we hit the per-process fd limit,
// commonly 1024). This is exactly the kind of resource that RAII exists to
// manage in C++ - hence this Socket class.
// ============================================================================


// ============================================================================
// Socket: RAII wrapper around a raw file descriptor.
// ============================================================================
// Responsibilities:
//   - Own exactly one fd (or no fd, if "empty"/moved-from).
//   - Guarantee close(fd) is called exactly once, when the Socket is
//     destroyed (or earlier, if close() is called manually).
//   - Be non-copyable (you cannot have two owners of the same fd - copying
//     an fd integer doesn't duplicate the OS resource, it just creates two
//     handles to the SAME resource, which would cause double-close bugs).
//   - Be movable (ownership of the fd can be transferred, e.g. when an
//     Acceptor "gives away" a newly-accepted connection fd to a Connection
//     object).
//
// This class deliberately knows NOTHING about HTTP, epoll, or threads.
// It is the lowest-level building block: "a safe integer that closes
// itself."
// ============================================================================
class Socket {
public:
    // ------------------------------------------------------------------
    // Sentinel value representing "no file descriptor owned".
    // Real fd's returned by the kernel are always >= 0, so -1 is a safe
    // "invalid/empty" marker.
    // ------------------------------------------------------------------
    static constexpr int kInvalidFd = -1;

    // ------------------------------------------------------------------
    // Default constructor: an "empty" Socket that owns no fd.
    // Useful as a placeholder before assigning a real socket via move.
    // ------------------------------------------------------------------
    Socket() noexcept : fd_(kInvalidFd) {}

    // ------------------------------------------------------------------
    // Construct a Socket that takes ownership of an already-created fd.
    //
    // This constructor is intentionally `explicit` to avoid accidental
    // implicit conversions from a raw int to a Socket (which could hide
    // ownership-transfer bugs).
    //
    // Marked `explicit` and takes ownership immediately: from this point
    // on, THIS Socket object is responsible for closing `fd`.
    // ------------------------------------------------------------------
    explicit Socket(int fd) noexcept : fd_(fd) {}

    // ------------------------------------------------------------------
    // Destructor: the heart of RAII.
    //
    // If we own a valid fd, close it. This runs automatically whenever
    // a Socket goes out of scope - whether that's normal flow, an early
    // `return`, or an exception unwinding the stack. This is the
    // guarantee that makes fd leaks impossible (as long as ownership is
    // tracked correctly via move semantics).
    // ------------------------------------------------------------------
    ~Socket() {
        close();
    }

    // ------------------------------------------------------------------
    // Copying is FORBIDDEN.
    //
    // Why: if Socket were copyable, two Socket objects could both think
    // they own fd 5. When the first one is destroyed, it calls close(5).
    // The second Socket now holds a "dangling" fd - any further use of it
    // is undefined behavior (and on Linux, fd 5 might already have been
    // re-issued by the kernel to a completely unrelated socket/file,
    // causing very confusing bugs where operations affect the wrong
    // resource).
    //
    // Deleting the copy constructor/assignment makes this a compile-time
    // error instead of a runtime disaster.
    // ------------------------------------------------------------------
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // ------------------------------------------------------------------
    // Moving is ALLOWED - this transfers ownership of the fd.
    //
    // After a move, the source Socket (`other`) is left in the "empty"
    // state (fd_ == kInvalidFd), so its destructor will safely do nothing.
    // The destination Socket now owns the fd and will close it.
    //
    // This is how, e.g., an Acceptor can `return Socket(client_fd)` from
    // a function and have that ownership cleanly transfer to the caller
    // without any double-close risk.
    // ------------------------------------------------------------------
    Socket(Socket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = kInvalidFd;  // disarm the moved-from object
    }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            // We're about to overwrite fd_, so close whatever we
            // currently own first (otherwise that fd would leak).
            close();
            fd_ = other.fd_;
            other.fd_ = kInvalidFd;  // disarm the moved-from object
        }
        return *this;
    }

    // ------------------------------------------------------------------
    // close(): release the owned fd early, if any.
    //
    // Safe to call multiple times - after the first call, fd_ becomes
    // kInvalidFd, so subsequent calls are no-ops. This idempotency is
    // important because the destructor also calls close().
    //
    // We deliberately ignore the return value of ::close(). On Linux,
    // close() can fail (e.g. EINTR), but retrying close() after EINTR is
    // actually unsafe (the fd may already have been released), so for an
    // educational server we simply attempt it once and move on.
    // ------------------------------------------------------------------
    void close() noexcept {
        if (fd_ != kInvalidFd) {
            ::close(fd_);
            fd_ = kInvalidFd;
        }
    }

    // ------------------------------------------------------------------
    // fd(): read-only access to the underlying file descriptor.
    //
    // Needed because all the actual syscalls (bind, listen, accept,
    // setsockopt, and later epoll_ctl) take a raw int fd, not a Socket
    // object. This class is a thin ownership wrapper, not a full
    // re-implementation of the socket API.
    // ------------------------------------------------------------------
    [[nodiscard]] int fd() const noexcept { return fd_; }

    // ------------------------------------------------------------------
    // isValid(): does this Socket currently own a real fd?
    // ------------------------------------------------------------------
    [[nodiscard]] bool isValid() const noexcept { return fd_ != kInvalidFd; }

private:
    int fd_;  // the owned file descriptor, or kInvalidFd if empty
};

}  // namespace miniginx::net