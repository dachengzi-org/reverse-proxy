#include "ProxyServer.h"
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>



#define MAX_EVENTS 10

// Constructor
ProxyServer::ProxyServer(int p) : port(p) {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    // int setsockopt(int sockfd, int level, int optname,
    // const void *optval, socklen_t optlen);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // sockaddr_in 是专门用来存储 IPv4 地址和端口信息的结构体
    struct sockaddr_in address;
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

void ProxyServer::start() {
    //
    // while(true) {
    //     // int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
    //     // 它会返回专属这个连接的一个新 socket file descriptor 给你！
    //     // 你突然有了两个 socket file descriptor！
    //     // 原本的 socket file descriptor 仍然正在 listen 之後的连线，
    //     // 而新建立的 socket file descriptor 则是在最後要准备给 send() 与 recv() 用的。
    //     int client_fd = accept(server_fd, nullptr, nullptr);
    //     if (client_fd >= 0) { // 如果还没发完
    //         handle_client(client_fd);
    //     }
    // }

    // int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    //    struct epoll_event ev;
    //    ev.events = EPOLLIN;     // 设置监听“可读事件”
    //    ev.data.fd = client_fd;  // 附带该 fd 信息，方便后续触发时提取
    //    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

    // int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

    // initialize epoll list and register server_fd to the epoll list
    int epoll_fd = epoll_create1(0);
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
        // 返回的n代表有几个事件触发了。阻塞等待。
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                // handles new connection
                int client_fd = accept(server_fd, nullptr, nullptr);
                if (client_fd >= 0) {
                    std::cout << "You have a new client connection: fd " << client_fd << "\n";
                    // register the new client_fd to epoll
                    struct epoll_event client_event;
                    client_event.events = EPOLLIN;
                    client_event.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event);
                }
            } else {
                // handles existing connection
                int client_fd = events[i].data.fd;
                handle_client(client_fd);
                // remove from epoll
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
            }
        }
    }
}

// 核心逻辑
void ProxyServer::handle_client(int client_fd) {
    char buffer[4096];
    memset(buffer, 0, sizeof(buffer));

    // 从客户端接收数据
    int bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read <= 0) {
        close(client_fd);
        return;
    }

    std::string request(buffer); // transfer to c++ string

    if (!request.empty()) {
        std::istringstream iss(request);
        std::string method, path, protocol;

        iss >> method >> path >> protocol;

        std::cout << ">>> 正在连接后端 8000 端口...\n";
        int backend_fd = connect_to_backend("127.0.0.1", 8000);

        // error handling
        if (backend_fd < 0) { 
            std::cout << "连不上后端，返回 502 Bad Gateway\n";
            std::string err = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
            send(client_fd, err.c_str(), err.length(), 0);
            close(client_fd);
            return;
        }
        
        // 发送给后端的代码
        // middleman
        std::string modified_request(buffer, bytes_read);

        size_t pos = modified_request.find("keep-alive");
        if (pos != std::string::npos) {
            modified_request.replace(pos, 10, "close");
        }
        // 把客户发给你的 HTTP 请求，转发给后端服务器
        send(backend_fd, modified_request.c_str(), modified_request.length(), 0);

        // 从后端接收数据
        char backend_buffer[8192];
        while (true) {
            memset(backend_buffer, 0, sizeof(backend_buffer));
            // get data from backend
            int backend_bytes = recv(backend_fd, backend_buffer, sizeof(backend_buffer) - 1, 0);

            if (backend_bytes == 0) {
                std::cout << "Backend has finished sending data. Closing the connection.\n";
                break;
            } else if (backend_bytes < 0) {
                std::cout << "A backend error occured.\n";
                break;
            }

            std::cout << "Sucessfully retrive backend data, size: " << backend_bytes << "bytes.\n";
            
            // send the data back to the client
            send(client_fd, backend_buffer, backend_bytes, 0);

        }
        
        close(backend_fd);
    }

    close(client_fd);
}

// This function is used to connect to a backend server.
// Given IP and port, returns a socket file descriptor if successful, or -1 otherwise.
int ProxyServer::connect_to_backend(const char* ip, int port) {
    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd < 0) {
        return -1;
    }

    struct sockaddr_in backend_addr;
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(port);
    // 把字符串 IP 转成二进制大端序格式
    inet_pton(AF_INET, ip, &backend_addr.sin_addr);

    if (connect(backend_fd, (sockaddr*)&backend_addr, sizeof(backend_addr)) < 0) {
        close(backend_fd);
        return -1;
    }

    return backend_fd;
}

// 通用的
// struct sockaddr {
//     unsigned short sa_family; // 协议族 (比如是 IPv4 还是 IPv6)，占 2 字节
//     char sa_data[14];         // 剩下的全当成不知名的数据，占 14 字节
// }; // 总共 16 字节


// // 内层结构体：专门用来装 32 位二进制 IP 地址
// struct in_addr {
//     uint32_t s_addr; // 真正的 32 位 IP 地址变量
// };

// // 外层结构体：用来装 IPv4 的完整网络信息
// struct sockaddr_in {
//     sa_family_t    sin_family; // 2 字节：协议族
//     in_port_t      sin_port;   // 2 字节：端口
//     struct in_addr sin_addr;   // 4 字节：嵌套了上面的结构体！
//     char           sin_zero[8];// 8 字节：填充位
// };