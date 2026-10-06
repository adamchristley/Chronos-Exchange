# Chronos Exchange

Chronos is a C++20 exchange simulator built to explore the systems problems behind low-latency trading infrastructure: binary network protocols, deterministic matching, lock-free handoff queues, real-time market-data publication, event sourcing, replay, CPU affinity, and tail-latency measurement.

This is an educational systems project, not a production exchange.

## What it implements

- **Price-time-priority matching engine** with partial fills and cancellation
- **Pre-trade risk checks** for quantity and price bounds
- **Nonblocking epoll TCP gateway** with partial-frame buffering, concurrent clients, and explicit network byte order
- **UDP market-data feed** for accepts, rejects, cancels, and trades
- **Three-stage multithreaded pipeline**: TCP gateway → matching engine → UDP publisher
- **Bounded lock-free SPSC ring buffers** between critical pipeline stages
- **Optional Linux CPU affinity** for gateway, matcher, and publisher threads
- **Append-only binary event log** containing inputs and emitted events
- **Deterministic replay tool** that rebuilds book state and verifies trade counts
- **Microbenchmark harness** reporting throughput plus p50/p95/p99 submit latency\n- **Concurrent TCP load generator** measuring client-observed end-to-end p50/p95/p99/p99.9 latency
- **Automated tests and GitHub Actions CI**

## Architecture

```text
                   TCP / binary protocol
Trading clients  ------------------------>
                                      +------------------+
                                      |  Order Gateway   |
                                      | epoll + framing  |
                                      +--------+---------+
                                               |
                                    lock-free SPSC queue
                                               |
                                               v
                                      +------------------+
                                      | Matching Engine  |
                                      | risk + book      |
                                      | price/time       |
                                      +---+----------+---+
                                          |          |
                               append-only|          |lock-free SPSC
                                   log    |          |
                                          v          v
                                      event log   UDP Publisher
                                                      |
                                                      v
                                             Market-data clients
```

The matching thread is the single writer for order-book state. This keeps matching deterministic and avoids locks on the book itself. The TCP gateway uses nonblocking sockets plus epoll and maintains a per-connection receive buffer, so split TCP frames and multiple messages per connection are handled without blocking the networking thread. Network ingestion and market-data publication run independently and communicate through bounded SPSC queues.

## Build

Linux is the primary target.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requirements: CMake 3.20+, a C++20 compiler, and POSIX sockets/pthreads.

## Run the distributed demo

Terminal 1:

```bash
./build/chronos_subscriber 9100
```

Terminal 2:

```bash
./build/chronos_server --log chronos.events
```

Terminal 3:

```bash
./build/chronos_client 127.0.0.1 9000 BUY  AAPL 10025 10 3
./build/chronos_client 127.0.0.1 9000 SELL AAPL 10000  5 4
```

Stop the server with Ctrl+C, then replay the exact input stream:

```bash
./build/chronos_replay chronos.events
```

The replay process reconstructs book state from order records and compares its trade count with the trade events recorded during the live run. A mismatch returns a nonzero exit code.

You can also replay only the prefix of a run, which is useful for time-travel debugging:

```bash
./build/chronos_replay chronos.events --until 5000
```

## CPU affinity

On Linux, pin the three pipeline threads to cores 1, 2, and 3:

```bash
./build/chronos_server --pin
```

This is intentionally optional because CPU topology differs across machines. The benchmark executable is in-process and does not pin by default.

## Benchmark

```bash
./build/chronos_bench 1000000
```

Example output format:

```text
orders=1000000
trades=...
seconds=...
throughput_orders_per_sec=...
latency_p50_ns=...
latency_p95_ns=...
latency_p99_ns=...
resting_orders=...
```

Benchmark numbers are hardware- and build-dependent. Run Release builds on an otherwise idle machine before quoting results.

### End-to-end network benchmark

The in-process benchmark isolates matching-engine cost. For a system-level measurement, run the exchange and the concurrent load generator in separate terminals:

```bash
./build/chronos_server --udp-host 127.0.0.1 --udp-port 9100
```

```bash
./build/chronos_loadgen --clients 8 --orders 100000
```

The load generator opens the requested number of TCP connections, synchronizes their start, sends binary order messages through the real epoll gateway, and correlates each order ID with the corresponding Accepted or Rejected UDP market-data event. It reports:

```text
clients=...
orders_requested=...
acks_received=...
acks_missing_or_dropped=...
send_rate_orders_per_sec=...
observed_end_to_end_orders_per_sec=...
latency_p50_us=...
latency_p95_us=...
latency_p99_us=...
latency_p99_9_us=...
latency_max_us=...
```

This measures the application-observed path from client send through TCP framing, gateway handoff, risk checks, matching, the market-data queue, UDP publication, and receipt by the load generator. Because acknowledgements currently travel over UDP, the tool explicitly reports missing/dropped acknowledgements instead of hiding them.

## Binary protocol

The TCP gateway receives fixed-size 48-byte order messages containing:

- magic + protocol version
- message type (new/cancel)
- order ID and client ID
- 8-byte symbol
- side
- integer price ticks
- quantity
- sender timestamp

Prices are represented as integer ticks rather than floating-point values, avoiding floating-point equality issues in the matching engine.

Market-data events are fixed-size UDP datagrams containing sequence number, event type, order IDs, symbol, price, quantity, timestamp, and rejection reason.

## Matching semantics

The server maintains an independent order book for each symbol. Within each book, bids are ordered highest-price first and asks lowest-price first. Within a price level, FIFO ordering enforces time priority.

Example:

```text
BUY  10 @ 100.00   (order 1)
BUY  10 @ 101.00   (order 2)
SELL 12 @  99.00   (order 3)
```

Order 3 first trades 10 units against order 2 at 101.00, then 2 units against order 1 at 100.00.

## Correctness strategy

The test suite covers:

- best-price execution
- FIFO time priority within one price level
- partial fills
- cancellation
- risk rejection
- ring-buffer full/empty behavior

The replay tool provides an additional end-to-end invariant: replaying the same input stream must regenerate the same number of trades and final top-of-book state.

## Roadmap

- per-symbol books and sharded matching threads
- snapshot + log recovery
- primary/replica event-stream replication
- client acknowledgements and sequence-gap detection
- io_uring gateway variant
- lock-free queue vs mutex queue benchmark
- eBPF/perf-based scheduler and syscall profiling
- fault-injection harness for disconnects, malformed frames, and process failure

## Why Chronos

The project is deliberately focused on mechanisms that are easy to hand-wave but hard to understand without building them: message framing, byte order, backpressure, queue ownership, memory ordering, deterministic state machines, tail latency, and the difference between concurrency and parallelism.
