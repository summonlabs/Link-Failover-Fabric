# Concurrency, ownership and shutdown audit

This audit was performed by inspecting ownership and call paths deliberately, not by relying on
tests. Every reproducible issue it found was fixed, and each fix is recorded below with the
reasoning that makes it correct.

## 1. Lock inventory

| Object | Lock | Guards |
| --- | --- | --- |
| `Engine::Impl` | `std::mutex mu` | all engine state: durable lineage, observation tables, publisher registry, decision log, pending directives |
| `Server::Impl` | `std::mutex registry_mu` | the session registry, the session thread list, the teardown flag |
| `Server::Impl` | `std::mutex wait_mu` + `std::condition_variable wait_cv` | the "run until stopped" wait |
| `Client::Impl` | `std::mutex mu` | the connection, the cached handshake answer, the session sequence |
| socket layer | `std::once_flag` | one-time process-wide network initialisation |

No `std::shared_mutex`, no reader/writer locks, no recursive mutexes.

## 2. Audit checklist

### 2.1 Read-lock then write-lock re-entry on the same lock
Not applicable: no read locks exist, and no mutex is recursive. Every lock is taken exactly once
per call path.

### 2.2 Write lock held across helper/callback paths that re-enter state
`Engine::Impl::mu` is held while `LineageStore::commit` runs its `apply` callback. Those
callbacks call only `store.note_attempt`, `store.note_fence`, `store.note_idempotency` and
`store.note_epoch`, none of which take `Engine::Impl::mu` or re-enter the engine. The
requirement is now documented at the `commit` declaration: **an apply callback must not re-enter
the store or the engine.**

`Store::commit` itself is never called with `registry_mu` or `wait_mu` held.

### 2.3 Event, log and callback invocation beneath internal locks
* `FrameDecoder::push` invokes its frame handler with **no** server lock held. The handler
  (`Server::Impl::handle_frame`) calls into the engine, which takes `Engine::Impl::mu`.
* `JournalReplayHooks::on_record` is invoked during store open, before any engine lock exists.
* `Server::Impl::request_stop` calls `wait_cv.notify_all()` while holding `wait_mu`. This is
  intentional and correct (the waiter re-checks its predicate), and it is the only notification
  site.
* No logging callback exists anywhere in the runtime; diagnostics are returned as values.

### 2.4 Joining workers while holding state they need
`Server::Impl::teardown` **swaps the session thread list out under `registry_mu` and then
releases the lock before joining.** Session threads take `registry_mu` when they mark their slot
free, so joining under the lock would deadlock. The accept thread is joined with no lock held.

### 2.5 Cancellation and shutdown with reversed lock ordering
The only nesting is `registry_mu` → `wait_mu` inside `request_stop`. No path takes `wait_mu`
and then `registry_mu`. `run()` takes `wait_mu` alone (released while waiting).
**Documented lock order: `registry_mu` before `wait_mu`; `Engine::Impl::mu` is never held
together with either.**

### 2.6 Blocked socket and thread teardown — **two defects found and fixed**
* **Defect A.** `accept()` was not released by `shutdown()` or `closesocket()` on a listening
  socket. Teardown waited on the accept thread until the platform's internal timeout.
* **Defect B.** `shutdown(SD_BOTH)` did **not** release a session thread blocked in `recv()`.
  Measured effect: `Server::shutdown()` returned in 6.7 ms, but the session thread only exited
  after **120.0 s**, so `run()` (and therefore process teardown) took two minutes.

  Fix: both loops now wait for readability with `select` and a 50 ms bound, then re-read their
  own stop flag. Shutdown latency is bounded by the poll interval instead of by a transport
  timeout, and the descriptor is never closed from another thread to force a wake-up. Measured
  after the fix: **81 ms**, a 1480× improvement, with no change to steady-state throughput.

  Residual: a `send` already in progress to a peer that stops reading drains or fails on its own.
  `request_stop` additionally issues `shutdown` on live session handles, which releases an
  in-flight `send` on most platforms; the residual window is bounded by the 256 KiB frame cap and
  the socket send buffer, and the peer is inside the trust boundary. This is a documented
  limitation, not a hidden one.

### 2.7 Cross-object mutex order inversion
There is no nesting between `Engine::Impl::mu`, `registry_mu` and `Client::Impl::mu`:
* a session thread takes `Engine::Impl::mu` (inside `handle_frame`) and, separately and later,
  `registry_mu` (when it exits);
* `teardown` holds nothing while joining;
* a `Client` in the same process speaks over a socket and never touches an `Engine` object.

### 2.8 Moved-from handle ownership — **one defect found and fixed**
* **Defect C.** The test harness' `Process::operator=` invoked its own destructor and then
  assigned to the destroyed object — undefined behaviour. Fixed by releasing the current
  process' resources in place. (Test-support code, but it would have corrupted process
  accounting in the multiprocess proofs.)

The runtime's movable types (`Engine`, `LineageStore`, `JournalWriter`, `Result<T>`) all null
their pointer on move and every accessor checks for the null state. `Server` and `Client` are
deliberately non-movable.

### 2.9 Close/shutdown races and double-close — **one defect found and fixed**
* **Defect D.** `SessionState::socket` was a plain field read and written by the accept thread,
  the stop path and the session thread. The stop path could shut down a handle the session thread
  had already closed and the operating system had already recycled.

  Fix: the handle is `std::atomic<SocketHandle>`; the **stop path only ever shuts down** (never
  closes, never clears the handle, and skips a value it observes as invalid), while the **session
  thread is the single owner of the close**, guarded by a `closed` atomic so exactly one close
  happens and no descriptor is leaked when both paths finish. Closing from the stop path would be
  worse than a stale shutdown: it could recycle a descriptor while the session thread is inside
  `select` on it.

* `Server::shutdown()` and `Engine::close()` are idempotent; `teardown` is fenced by a
  `torn_down` flag taken under `registry_mu`; `JournalWriter::close` nulls its implementation;
  `Client::close` checks the handle. All are safe to call twice.

### 2.10 Callbacks retaining references beyond lock lifetime
Every callback is invoked synchronously within the scope that owns the referenced state:
* the engine's `apply` lambdas run inside `commit`, with `impl` and locals alive for the whole
  call;
* `LineageStore::open`'s replay hook captures the implementation under construction and runs
  during `open`;
* the frame decoder's handler captures the session by `shared_ptr` and a `close_session` flag by
  reference, both used synchronously;
* the accept thread captures a raw `Impl*` that outlives it because the thread is joined in
  `teardown` before the implementation is destroyed.

### 2.11 References into mutable containers held across a commit — **one defect found and fixed**
* **Defect E.** `Engine::request_rollback` kept a pointer into the attempt table (`found`) and used
  it *after* a commit that can reallocate that table. Fixed by copying the subject, generation and
  attempt sequence before committing.

  The same audit found and fixed an earlier instance in `request_failover`, where a pointer into
  the topology link list was dereferenced after commits; the definition is now copied.

  `record_application`, `record_verification` and `resolve_interrupted` were checked and already
  copy the record before committing.

### 2.12 Lifetimes handed to callers — **one defect found and fixed**
* **Defect F.** `Client::hello()` returned a reference to cached handshake state that `ping()`
  and `refresh_handshake()` mutate. It now returns by value, as `Engine::policy()` and
  `Engine::topology()` already did.

### 2.13 Durability under concurrent load — **one defect found and fixed**
* **Defect G.** Every refusal of an already-authoritative generation performed a durable
  `DecisionCommit` with an fsync. A client could therefore spend one fsync per request without
  changing any authority — an unbounded cost and a denial-of-service vector. Measured effect:
  **95 decisions/s**.

  Fix: decisions that only restate state already on the medium (already-authoritative refusal,
  policy-disabled refusal, expectation mismatch, idempotent replay, duplicate or late completion)
  are returned with `durable = false` and `lineage_seq = 0`. Durable records are now exactly
  those that change what the next boot would observe. Measured after the fix: **10 574
  decisions/s**, with the same behaviour asserted by the engine and multiprocess suites.

## 3. Ownership rules the code now relies on

1. A journal record is appended, flushed to the medium, and only then applied in memory. A failed
   append leaves memory untouched and marks the store permanently failed.
2. An apply callback never re-enters the store or the engine.
3. `Engine::Impl::mu` is the only lock the engine takes, and it is never held together with a
   server lock.
4. The session thread owns its socket. The stop path only signals and shuts down.
5. Joins happen with no lock held, and never from the thread being joined.
6. No reference or pointer into a mutable container survives a call that can mutate it.
7. No callback reaches into state that outlives the call that invoked it.
8. Decision durability is a property of what changes, not of what is reported.

## 4. Unresolved, documented risks

* A `send` in progress to a non-reading peer is not forcibly interrupted (section 2.6).
* The stop path's `shutdown` may in principle observe a session handle that the session thread
  has just invalidated. The window is a single call, the handle is read atomically, and the
  session thread is the only closer, so the worst case is a no-op `shutdown` on an invalid
  handle. It cannot close, reuse or read another connection's data.
* Physical fabric behaviour, sanitizer coverage and authenticated transport are out of scope as
  recorded in the README's REAL / SYNTHETIC / UNSUPPORTED matrix.
