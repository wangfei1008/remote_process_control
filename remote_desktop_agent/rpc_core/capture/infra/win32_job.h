#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace win32 {

class Job {
public:
    Job() = default;
    ~Job();

    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    Job(Job&& other) noexcept;
    Job& operator=(Job&& other) noexcept;

    bool create(const wchar_t* name = nullptr);
    void reset();

    bool set_kill_on_job_close(bool enable);
    bool assign_process(HANDLE process_handle);

    std::vector<DWORD> query_process_ids() const;

    HANDLE handle() const { return m_h; }
    bool valid() const { return m_h != nullptr; }

private:
    HANDLE m_h = nullptr;
};

} // namespace win32

