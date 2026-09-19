#pragma once
#include <unordered_map>
#include <string>

struct ConnectionState {
    int peer_fd;            // 绑定的另一端管子
    std::string out_buffer; // 还没发完的数据队列
    bool is_paused;          // 是否暂停发送数据
};

class ProxyServer {
private:
    int server_fd; // 总机电话
    int port;
    int epoll_fd; // epoll 文件描述符
    std::unordered_map<int, ConnectionState> fd_states;

public:
    // Constructors
    ProxyServer(int port);
    ~ProxyServer();
    void start();

private:
    int connect_to_backend(const char* ip, int port);
    void send_data(int fd, const char* data, int len);
};