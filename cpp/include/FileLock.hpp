#pragma once
// FileLock.hpp - lightweight cross-platform file lock wrapper (declaration only)
//
// Keep header minimal so it can be included widely without pulling system headers.
//
// Usage:
//   pylabhub::fileutil::FileLock flock(path, pylabhub::fileutil::LockMode::NonBlocking);
//   if (!flock.valid()) { auto ec = flock.error_code(); /* inspect */ }

#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <string>
#endif


namespace pylabhub::fileutil {

    enum class LockMode {
        Blocking,
        NonBlocking
    };

#if defined(_WIN32)
    std::wstring win32_to_long_path(const std::filesystem::path& p_in);
    std::wstring win32_make_unique_suffix();
#endif

    class FileLock {
    public:
        explicit FileLock(const std::filesystem::path& path, LockMode mode = LockMode::Blocking);
        ~FileLock();

        FileLock(const FileLock&) = delete;
        FileLock& operator=(const FileLock&) = delete;

        FileLock(FileLock&& other) noexcept;
        FileLock& operator=(FileLock&& other) noexcept;

        bool valid() const noexcept;
        std::error_code error_code() const noexcept;

    private:
        void open_and_lock(LockMode mode);

        std::filesystem::path _path;
        bool _valid{ false };
        std::error_code _ec;

#ifdef _WIN32
        void* _handle{ nullptr }; // actually HANDLE
#else
        int _fd{ -1 };
#endif
    };

} // namespace pylabhub::fileutil
