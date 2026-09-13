#include "ProxyServer.h"

int main() {
    ProxyServer proxy(3490);
    proxy.start();
    
    return 0;
}