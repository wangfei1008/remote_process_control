#include "win32_job.h"

#include <vector>
#include <iostream>

namespace win32 {

Job::~Job() { reset(); }

Job::Job(Job&& other) noexcept : m_h(other.m_h) { other.m_h = nullptr; }

Job& Job::operator=(Job&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    m_h = other.m_h;
    other.m_h = nullptr;
    return *this;
}

bool Job::create(const wchar_t* name)
{
    reset();
    m_h = ::CreateJobObjectW(nullptr, name);
    return (m_h != nullptr);
}

void Job::reset()
{
    if (m_h) {
        ::CloseHandle(m_h);
        m_h = nullptr;
    }
}

bool Job::set_kill_on_job_close(bool enable)
{
    if (!m_h) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags =
        enable ? JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE : 0;
    return ::SetInformationJobObject(
               m_h, JobObjectExtendedLimitInformation, &info, sizeof(info)) != FALSE;
}

bool Job::assign_process(HANDLE process_handle)
{
    if (!m_h || !process_handle) return false;
    BOOL bret = AssignProcessToJobObject(m_h, process_handle);
    if (bret == FALSE) {
        DWORD error = GetLastError();
        std::cout << "AssignProcessToJobObject failed, error=" << error << std::endl;
        return false;
    }
    return true;
}

std::vector<DWORD> Job::query_process_ids() const
{
    std::vector<DWORD> out;
    if (!m_h)
    {
        std::cout << "query_process_ids called on invalid job handle" << std::endl;
        return out;
    }

    // JobObjectBasicProcessIdList does not support nullptr/length 0 to probe size (ERROR_BAD_LENGTH).
    // Use at least sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST), then grow on ERROR_MORE_DATA or when
    // NumberOfProcessIdsInList < NumberOfAssignedProcesses (MSDN).
    // Use uint8_t buffer to avoid any STL specialization/link issues with std::byte
    // across mixed third-party libraries.
    std::vector<uint8_t> buf(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST));

    for (;;) {
        DWORD return_length = 0;
        const BOOL ok = ::QueryInformationJobObject( m_h, JobObjectBasicProcessIdList, buf.data(), static_cast<DWORD>(buf.size()), &return_length);

        if (ok != FALSE) {
            auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buf.data());
            if (list->NumberOfProcessIdsInList < list->NumberOfAssignedProcesses) {
                const size_t required = FIELD_OFFSET(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList) + static_cast<size_t>(list->NumberOfAssignedProcesses) * sizeof(ULONG_PTR);
                // Avoid Windows.h max macro expanding std::max
                const size_t next = required > buf.size() + sizeof(ULONG_PTR) ? required : buf.size() + sizeof(ULONG_PTR);
                buf.resize(next);
                continue;
            }
            const auto n = static_cast<size_t>(list->NumberOfProcessIdsInList);
            out.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                const ULONG_PTR pid_raw = list->ProcessIdList[i];
                out.push_back(static_cast<DWORD>(pid_raw));
                std::cout << "Job contains process ID: " << pid_raw << std::endl;
            }
            return out;
        }

        const DWORD err = ::GetLastError();
        if (err == ERROR_MORE_DATA && return_length > buf.size()) {
            buf.resize(return_length);
            continue;
        }
        std::cout << "QueryInformationJobObject failed, error=" << err << std::endl;
        return out;
    }
}

} // namespace win32

