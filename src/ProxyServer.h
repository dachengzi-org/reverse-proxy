#pragma once
#include <string>

class ProxyServer {
private:
    int server_fd; // 总机电话
    int port;

public:
    // Constructors
    ProxyServer(int port);

    ~ProxyServer();

    void start();

private:
    void handle_client(int client_fd);
    int connect_to_backend(const char* ip, int port);
};