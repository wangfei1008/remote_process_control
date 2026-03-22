#include <iostream>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <future>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <rtc/websocket.hpp>
#include "webrtc_socket.h"
#include "process_manager.h"
using namespace std;
using namespace std::chrono_literals;
using json = nlohmann::json;


// Ö÷º¯Êý
int main() 
{

    WebRTCSocket ws;
    ws.start("127.0.0.1", 9090);

    while (true) {
        string id;
        cout << "Enter to exit" << endl;
        cin >> id;
        cin.ignore();
        cout << "exiting" << endl;
        break;
    }
    return 0;
}