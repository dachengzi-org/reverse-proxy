#include "ProxyServer.h"
#include "picohttpparser.h"

#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cerrno>

#define MAX_EVENTS 10

// Set the file descriptor to non blocking mode
void set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Constructor
ProxyServer::ProxyServer(int p) : port(p) {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    set_non_blocking(server_fd);
    
    int yes = 1;
    // int setsockopt(int sockfd, int level, int optname,
    // const void *optval, socklen_t optlen);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // sockaddr_in 是专门用来存储 IPv4 地址和端口信息的结构体
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET; //指定协议族/地址族
    address.sin_addr.s_addr = INADDR_ANY; // 指定要绑定哪些IP地址
    address.sin_port = htons(port); // 设置端口并指定为大端序

    // int bind(int sockfd, struct sockaddr *my_addr, int addrlen);
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 10);
    std::cout << "Proxy server sucessfully started on port " << port << ".\n";
}

ProxyServer::~ProxyServer() {
    close(server_fd);
}

// This function is used to connect to a backend server.
// Given IP and port, returns a socket file descriptor if successful, or -1 otherwise.
int ProxyServer::connect_to_backend(const char* ip, int port) {
    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd < 0) return -1;

    struct sockaddr_in backend_addr;
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &backend_addr.sin_addr);

    // connect to a server
    if (connect(backend_fd, (sockaddr*)&backend_addr, sizeof(backend_addr)) < 0) {
        close(backend_fd);
        return -1;
    }

    // After the connection is established,
    // set the socket to non-blocking mode for epoll
    set_non_blocking(backend_fd);
    return backend_fd;
}

// Core function of the proxy server. 
// It uses epoll to handle multiple connections efficiently. 
void ProxyServer::start() {
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll create failed");
        return;
    }

    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl failed");
        return;
    }

    // event loops
    struct epoll_event events[MAX_EVENTS];
    std::cout << "Epoll loop sucessfully started\n";

    while (true) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            int triggered_fd = events[i].data.fd;
            uint32_t trigger_events = events[i].events; // gettriggered events

            // Channel 1: Handle outgoing data (EPOLLOUT)
            if (trigger_events & EPOLLOUT) {
                if (fd_states.count(triggered_fd)) {
                    auto& state = fd_states[triggered_fd];
                    if (!state.out_buffer.empty()) {
                        int bytes = send(triggered_fd, state.out_buffer.data(), state.out_buffer.size(), MSG_NOSIGNAL);
                        if (bytes > 0) {
                            state.out_buffer.erase(0, bytes); 
                        }
                        else if (bytes < 0 && errno != EAGAIN) {
                            state.out_buffer.clear(); // clear it
                            if (state.peer_fd != -1 && fd_states.count(state.peer_fd)) {
                                fd_states[state.peer_fd].peer_fd = -1;
                            }
                            state.peer_fd = -1; 
                        }
                    }
                    
                    if (state.out_buffer.empty()) {
                        // if buffer is empty, we can resume reading from the peer
                        int peer = state.peer_fd;
                        if (peer != -1 && fd_states.count(peer) && fd_states[peer].is_paused) {
                            struct epoll_event ev;
                            ev.events = EPOLLIN;
                            ev.data.fd = peer;
                            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, peer, &ev); // re-register epoll
                            fd_states[peer].is_paused = false;
                        }

                        // If we have a peer, we should not close the connection too early,
                        // because the peer might still have data to send.
                        if (state.peer_fd == -1) {
                            // if the peer is gone, we can safely close this fd
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, triggered_fd, nullptr);
                            close(triggered_fd);
                            fd_states.erase(triggered_fd);
                        } else {
                            // if we still have a peer, we should just stop listening for EPOLLOUT events
                            struct epoll_event ev;
                            ev.events = EPOLLIN; 
                            ev.data.fd = triggered_fd;
                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, triggered_fd, &ev);
                        }
                    }
                }
            }

            // Channel 2: Accept incoming connections (EPOLLIN)
            if (triggered_fd == server_fd) {
                int client_fd = accept(server_fd, nullptr, nullptr);
                if (client_fd >= 0) {
                    set_non_blocking(client_fd);

                    struct epoll_event ev;
                    ev.events = EPOLLIN;
                    ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);

                    // Set the initial state for new connection
                    fd_states[client_fd] = {-1, "", false}; 
                }
                continue;
            }

            // Channel 3: Read data and forward it to the peer (EPOLLIN)
            if (trigger_events & EPOLLIN) {
                char buffer[8192];
                int bytes = recv(triggered_fd, buffer, sizeof(buffer) - 1, 0);

                if (bytes > 0) {
                    auto& state = fd_states[triggered_fd];

                    if (state.peer_fd == -1) {
                        // first time receiving data from this fd,
                        // we need to parse the HTTP request                        
                        const char *method, *path;
                        size_t method_len, path_len;
                        int minor_version;
                        struct phr_header headers[100];
                        size_t num_headers = 100;
                        
                        // picohttpparser
                        int pret = phr_parse_request(buffer, bytes, &method, &method_len, &path, &path_len,
                                                     &minor_version, headers, &num_headers, 0);
                        
                        if (pret > 0) {
                            // parsing successful
                            std::string req_path(path, path_len);
                            std::cout << "Got a request: " << std::string(method, method_len) 
                                      << " " << req_path << "\n";

                            // rebuild the request
                            std::string new_req(method, method_len);
                            new_req += " ";
                            new_req += req_path;
                            new_req += " HTTP/1." + std::to_string(minor_version) + "\r\n";

                            for (size_t i = 0; i < num_headers; ++i) {
                                std::string h_name(headers[i].name, headers[i].name_len);
                                std::string h_val(headers[i].value, headers[i].value_len);
                                
                                if (h_name == "Connection" || h_name == "connection") {
                                    h_val = "close";
                                }
                                new_req += h_name + ": " + h_val + "\r\n";
                            }
                            new_req += "\r\n"; // end of headers

                            // append body if any
                            if (bytes > pret) {
                                new_req.append(buffer + pret, bytes - pret);
                            }

                            // request to backend server
                            int backend_fd = connect_to_backend("127.0.0.1", 8000);
                            
                            if (backend_fd >= 0) {
                                struct epoll_event ev;
                                ev.events = EPOLLIN;
                                ev.data.fd = backend_fd;
                                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, backend_fd, &ev);

                                fd_states[triggered_fd].peer_fd = backend_fd;
                                fd_states[backend_fd] = {triggered_fd, "", false};

                                // send the rebuilt request to the backend
                                send_data(backend_fd, new_req.c_str(), new_req.length());
                            } else {
                                std::string err = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
                                send_data(triggered_fd, err.c_str(), err.length());
                            }
                        } else if (pret == -1) {
                            // not a valid HTTP request
                            close(triggered_fd);
                            fd_states.erase(triggered_fd);
                        } else if (pret == -2) {
                            // discard the request if it's incomplete for now
                            close(triggered_fd);
                            fd_states.erase(triggered_fd);
                        }
                    } else {
                        // established connection
                        send_data(state.peer_fd, buffer, bytes);

                        // If the peer's out_buffer exceeds 5MB, 
                        // pause reading from the current fd to prevent memory overflow
                        if (fd_states[state.peer_fd].out_buffer.size() > 5 * 1024 * 1024) {
                            if (!fd_states[triggered_fd].is_paused) {
                                // stop epoll listening
                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, triggered_fd, nullptr);
                                fd_states[triggered_fd].is_paused = true;
                            }
                        }
                    }
                } else if (bytes == 0 || (bytes < 0 && errno != EAGAIN)) {
                    // Clean up the connection and its peer if it exists
                    int peer_fd = fd_states[triggered_fd].peer_fd;

                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, triggered_fd, nullptr);
                    close(triggered_fd);
                    fd_states.erase(triggered_fd);

                    if (peer_fd != -1 && fd_states.count(peer_fd)) {
                        if (fd_states[peer_fd].out_buffer.empty()) {
                            // if peer's out_buffer is empty, we can safely close it
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, peer_fd, nullptr);
                            close(peer_fd);
                            fd_states.erase(peer_fd);
                        } else {
                            // if peer's out_buffer is not empty, 
                            // we should not close it yet
                            fd_states[peer_fd].peer_fd = -1; 
                        }
                    }
                }
            }
        }
    }
}

void ProxyServer::send_data(int fd, const char* data, int len) {
    if (fd_states.find(fd) == fd_states.end()) return;
    auto& state = fd_states[fd];

    int total_sent = 0;

    // if the queue is empty, that means we can send
    if (state.out_buffer.empty()) {
        total_sent = send(fd, data, len, MSG_NOSIGNAL);
        if (total_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                total_sent = 0; // buffer is full
            } else {
                return; // fatal error occured
            }
        }
    }

    // if we haven't sent all the data, append the rest to the out_buffer
    if (total_sent < len) {
        state.out_buffer.append(data + total_sent, len - total_sent);

        // register the fd to epoll for EPOLLOUT event
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }
}
