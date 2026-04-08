#include "app/rpc_remote_application.h"
#include "input/input_controller.h"
#include "app/runtime_config.h"

#include <iostream>
#include <string>

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          程序入口：初始化输入环境，启动远程桌面 Agent 并连接信令
/// @参数
///          无
/// @返回值
///          0：正常退出
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
int main()
{
    input_controller::ensure_process_dpi_awareness();

    rpc_remote_application app;
    const std::string signaling_ip = runtime_config::get_string("RPC_SIGNALING_IP", "127.0.0.1");
    const int signaling_port = runtime_config::get_int("RPC_SIGNALING_PORT", 9090);
    app.run(signaling_ip, signaling_port);

    std::cout << "[app] exit" << std::endl;
    return 0;
}
