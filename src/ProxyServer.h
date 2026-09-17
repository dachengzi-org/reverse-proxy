#pragma once
#include <unordered_map>

class ProxyServer {
private:
    int server_fd; // 总机电话
    int port;
    std::unordered_map<int, int> backend_to_client;
    std::unordered_map<int, int> client_to_backend;

public:
    // Constructors
    ProxyServer(int port);

    ~ProxyServer();

    void start();

private:
    void handle_client(int client_fd);
    int connect_to_backend(const char* ip, int port);
};