#pragma once
#include <chrono>
#include <cstdint>

inline uint64_t rpc_unix_epoch_ms()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}
