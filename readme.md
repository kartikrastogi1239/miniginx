# Mini Nginx — C++20 HTTP Server

> An educational, ground-up implementation of a Linux HTTP server inspired by Nginx's architecture.
> Built in phases, each adding one layer of the stack.

---

## Progress

| Phase | Topic | Status |
|-------|-------|--------|
| 1 | TCP Networking Foundation | ✅ Complete |
| 2 | Connection Abstraction & I/O | ✅ Complete |
| 3 | Event Loop & epoll | 🔜 Next |

---

## Architecture (Phases 1 & 2)

```
┌─────────────────────────────────────────────────┐
│                   main.cpp                      │
│         accept loop · handleClient()            │
└────────────────────┬────────────────────────────┘
                     │ creates
                     ▼
┌─────────────────────────────────────────────────┐
│               ServerSocket                      │
│  socket() → setsockopt() → bind() → listen()   │
│  accept() → returns (Socket, client IP)         │
└────────────────────┬────────────────────────────┘
                     │ moves fd into
                     ▼
┌─────────────────────────────────────────────────┐
│                Connection                       │
│  State: Connected | Closed | Error              │
│                                                 │
│  read()       single recv() call                │
│  readUntil()  accumulate until delimiter        │
│  write()      send-all loop over send()         │
│  close()      explicit shutdown                 │
└────────────────────┬────────────────────────────┘
                     │ owns
                     ▼
┌─────────────────────────────────────────────────┐
│                  Socket                         │
│  RAII wrapper · non-copyable · movable          │
│  ~Socket() → ::close(fd)  (guaranteed)          │
└─────────────────────────────────────────────────┘
```

---

## Current Features

- **`Socket`** — RAII wrapper around a Linux file descriptor. Non-copyable, movable. Guarantees `close()` on destruction with no possibility of double-close or fd leak.
- **`ServerSocket`** — Encapsulates the full server setup sequence: `socket()` → `setsockopt(SO_REUSEADDR)` → `bind()` → `listen()`. Throws on failure; a successfully constructed object is always ready to accept.
- **`Connection`** — Per-client abstraction owning the accepted fd. Tracks lifecycle state (`Connected / Closed / Error`), stores the peer IP, and exposes three I/O primitives:
  - `read(n)` — single `recv()`, surfaces all four outcomes (data, EOF, EINTR, error) via a typed `ReadResult`.
  - `readUntil(delimiter, max)` — accumulates bytes across multiple `recv()` calls until a framing delimiter is found; safe against unbounded growth.
  - `write(data)` — write-all loop over `send()` with `MSG_NOSIGNAL`; handles partial writes and suppresses `SIGPIPE`.

---

## What I Learned

**File descriptors are scarce OS resources.** Every `socket()` call allocates a kernel data structure referenced by a small integer. Forgetting to `close()` one leaks it until process exit. RAII (`Socket`) makes leaks structurally impossible — the destructor fires whether the scope exits normally, via early `return`, or through exception unwinding.

**TCP is a byte stream, not a message protocol.** `recv()` returning `n < len` is normal, not an error. Two `send()` calls may coalesce into one `recv()`, or one `send()` may arrive across three. Any protocol layered on TCP must define its own framing. `readUntil()` demonstrates the universal pattern: accumulate bytes, search for the application-level boundary marker, repeat.

**Partial writes are real.** `send()` returns the number of bytes the kernel *accepted into its send buffer* — not the number delivered to the peer. When that buffer is full, `send()` in blocking mode accepts fewer bytes than requested. A naive single `send()` call silently loses the remainder. The write-all loop fixes this.

**`MSG_NOSIGNAL` is not optional.** Writing to a connection whose peer has closed raises `SIGPIPE`, whose default handler kills the process silently. Passing `MSG_NOSIGNAL` to every `send()` converts that signal into a catchable `-1 / EPIPE` return value.

**Guard the state machine.** A `Connection` in `Error` or `Closed` state must refuse further I/O immediately, not attempt a syscall on a potentially recycled fd. The `State` enum enforces this at every entry point.

---

## Next Phase

Phase 3 introduces a non-blocking event loop using `epoll`, allowing a single thread to manage thousands of concurrent connections without blocking on any individual client.