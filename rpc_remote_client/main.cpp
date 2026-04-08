#include <iostream>
#include <string>
#include "transport/webrtc_socket.h"
#include "common/runtime_config.h"

int main()
{
    WebRTCSocket ws;
    const std::string signaling_ip = runtime_config::get_string("RPC_SIGNALING_IP", "127.0.0.1");
    const int signaling_port = runtime_config::get_int("RPC_SIGNALING_PORT", 9090);
    ws.start(signaling_ip, signaling_port);

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
