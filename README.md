# C++ Reverse Proxy Server

A lightweight, event-driven reverse proxy server built from scratch in C++17.

I built this project to deepen my understanding of Linux system programming, network protocols, and how web servers handle high concurrency. Instead of relying on traditional blocking I/O or multi-threading (such as `fork()`), this proxy is implemented using a single-threaded Reactor pattern based on `epoll`.

## Technical Highlights

* **Event-Driven Core:** Uses Linux `epoll` and non-blocking sockets to efficient manage concurrent TCP connections on a single thread.
* **Connection State Management:** Maintains a state machine to safely pair client and backend file descriptors, ensuring reliable bidirectional data forwarding.
* **HTTP Parsing:** Integrates `picohttpparser` to accurately parse incoming HTTP headers and rewrite specific fields (e.g., converting `keep-alive` to `close`) before routing to the backend.
* **Edge Case Handling:** Sucessfully handle unexpected TCP disconnects, half-closed connections (EOF), and `SIGPIPE` signals without crashing or leaking file descriptors.

## Build and Run

This project uses CMake for out-of-source builds.

```bash
# 1. Clone the repository
git clone https://github.com/dachengzi-org/reverse-proxy.git
cd reverse-proxy

# 2. Build
mkdir build && cd build
cmake ..
make

# 3. Run
./proxy

```

## Future Roadmap

* **Multi-threading:** Transition from a single-threaded Reactor to a Multi-Reactor model.
* **Configuration File Parser:** Load routing rules from a JSON/YAML configuration instead of hard-coded ones.
* **TLS/SSL Termination:** Support HTTPS client connections while maintaining plain HTTP with backends.

## Author

Jimmy Yu  
