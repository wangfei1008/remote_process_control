#include <iostream>
#include <string>
#include "transport/webrtc_socket.h"

int main()
{
    WebRTCSocket ws;
    ws.start("127.0.0.1", 9090);

    while (true) {
        std::string id;
        std::cout << "Enter to exit" << std::endl;
        std::cin >> id;
        std::cin.ignore();
        std::cout << "exiting" << std::endl;
        break;
    }
    return 0;
}
