// FileLock.cpp - implementation for FileLock
// Compile this file into your library/object and link with code that includes FileLock.hpp

#include "FileLock.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <random>
#include <chrono>
#include <sstream>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace pylabhub::fileutil {

#if defined(_WIN32)
	// Helper: convert a path to the Windows long-path form.
	// If the input is already prefixed with \\?\ it is returned unchanged.
	// If the path is a UNC path (starts with \\server\share), it is converted to \\?\UNC\server\share\...
	// Otherwise it's converted to absolute path and prefixed with \\?\ so that long paths are supported.

	static inline std::wstring normalize_backslashes(std::wstring s) {
		for (auto& c : s) if (c == L'/') c = L'\\';
		return s;
	}

	std::wstring win32_to_long_path(const std::filesystem::path& p_in) {
		std::filesystem::path abs = p_in;
		if (!abs.is_absolute()) {
			abs = std::filesystem::absolute(abs);
		}
		std::wstring ws = abs.wstring();
		ws = normalize_backslashes(ws);

		// If already long path, return as-is
		if (ws.rfind(L"\\\\?\\", 0) == 0) {
			return ws;
		}

		// UNC path? begins with \\server\share
		if (ws.rfind(L"\\\\", 0) == 0) {
			// strip leading "\\"
			std::wstring rest = ws.substr(2);
			// prefix with "\\?\UNC\"
			return std::wstring(L"\\\\?\\UNC\\") + rest;
		}

		// Normal absolute path: prefix with "\\?\"
		return std::wstring(L"\\\\?\\") + ws;
	}

	// Helper: generate a reasonably-unique suffix string (PID, threadid, time, random)
	std::wstring win32_make_unique_suffix() {
		// high-resolution time stamp
		auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		DWORD pid = GetCurrentProcessId();
		DWORD tid = GetCurrentThreadId();

		// random value
		std::random_device rd;
		std::mt19937_64 gen(rd());
		uint64_t r = gen();

		std::wstringstream ss;
		ss << L"." << pid << L"." << tid << L"." << now << L"." << std::hex << r;
		return ss.str();
	}
	// Helper: convert a Win32 error code to a UTF-8 string message
	std::string win32_err_to_string(DWORD err) {
		LPWSTR buf = nullptr;
		DWORD len = FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPWSTR)&buf, 0, nullptr);
		std::string out;
		if (len && buf) {
			// convert wide to UTF-8
			int size_needed = WideCharToMultiByte(CP_UTF8, 0, buf, (int)len, nullptr, 0, nullptr, nullptr);
			if (size_needed) {
				out.resize(size_needed);
				WideCharToMultiByte(CP_UTF8, 0, buf, (int)len, &out[0], size_needed, nullptr, nullptr);
				// trim trailing CRLF/spaces
				while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
			}
			LocalFree(buf);
		}
		else {
			out = "Unknown error " + std::to_string(err);
		}
		return out;
	}
#endif


	FileLock::FileLock(const std::filesystem::path& path, LockMode mode)
		: _path(path), _valid(false), _ec()
	{
		open_and_lock(mode);
	}

	FileLock::~FileLock() {
#ifdef _WIN32
		if (_handle) {
			CloseHandle(static_cast<HANDLE>(_handle));
			_handle = nullptr;
		}
#else
		if (_fd != -1) {
			::close(_fd);
			_fd = -1;
		}
#endif
	}

	FileLock::FileLock(FileLock&& other) noexcept
		: _path(std::move(other._path)), _valid(other._valid), _ec(other._ec)
	{
#ifdef _WIN32
		_handle = other._handle;
		other._handle = nullptr;
#else
		_fd = other._fd;
		other._fd = -1;
#endif
		other._valid = false;
		other._ec.clear();
	}

	FileLock& FileLock::operator=(FileLock&& other) noexcept {
		if (this == &other) return *this;

#ifdef _WIN32
		if (_handle) {
			CloseHandle(static_cast<HANDLE>(_handle));
			_handle = nullptr;
		}
#else
		if (_fd != -1) {
			::close(_fd);
			_fd = -1;
		}
#endif

		_path = std::move(other._path);
		_valid = other._valid;
		_ec = other._ec;

#ifdef _WIN32
		_handle = other._handle;
		other._handle = nullptr;
#else
		_fd = other._fd;
		other._fd = -1;
#endif

		other._valid = false;
		other._ec.clear();
		return *this;
	}

	bool FileLock::valid() const noexcept { return _valid; }
	std::error_code FileLock::error_code() const noexcept { return _ec; }

	void FileLock::open_and_lock(LockMode mode) {

		// compute a stable base name for the lock file
		std::string fname = _path.filename().string();
		// If filename is empty or just "." (happens for paths like "/" or ""), use parent folder name
		if (fname.empty() || fname == "." || fname == "..") {
			fname = _path.parent_path().filename().string();
		}
		// If still empty (e.g. parent path ended up empty too), use a safe fallback
		if (fname.empty()) fname = "config";

		// Now build lockfile as <parentdir>/<fname>.lock
		auto lockpath = _path.parent_path() / (fname + ".lock");

#ifdef _WIN32
		// Use wide string path for Win32 API
		std::wstring wlock = lockpath.wstring();
		HANDLE h = CreateFileW(
			wlock.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);

		if (h == INVALID_HANDLE_VALUE) {
			DWORD err = GetLastError();
			_ec = std::error_code(static_cast<int>(err), std::system_category());
			_valid = false;
			_handle = nullptr;
			std::cerr << "FileLock: CreateFileW failed: " << win32_err_to_string(err) << " (" << err << ")\n";
			return;
		}

		OVERLAPPED ov = {};
		DWORD flags = LOCKFILE_EXCLUSIVE_LOCK; // request exclusive lock
		if (mode == LockMode::NonBlocking) flags |= LOCKFILE_FAIL_IMMEDIATELY;

		BOOL ok = LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov);
		if (!ok) {
			DWORD err = GetLastError();
			CloseHandle(h);
			_ec = std::error_code(static_cast<int>(err), std::system_category());
			_valid = false;
			_handle = nullptr;
			std::cerr << "FileLock: LockFileW failed: " << win32_err_to_string(err) << " (" << err << ")\n";
			return;
		}

		_handle = static_cast<void*>(h);
		_valid = true;
		_ec.clear();

#else
#ifdef O_CLOEXEC
		int fd = ::open(lockpath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
#else
		int fd = ::open(lockpath.c_str(), O_RDWR | O_CREAT, 0666);
#endif
		if (fd == -1) {
			int err = errno;
			_ec = std::error_code(err, std::generic_category());
			_fd = -1;
			_valid = false;
			return;
		}
#ifndef O_CLOEXEC
		else { // set close-on-exec if O_CLOEXEC not available
			int flags = fcntl(fd, F_GETFD);
			if (flags != -1) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
		}
#endif


		int operation = LOCK_EX;
		if (mode == LockMode::NonBlocking) operation |= LOCK_NB;

		if (flock(fd, operation) == -1) {
			int err = errno;
			_ec = std::error_code(err, std::generic_category());
			::close(fd);
			_fd = -1;
			_valid = false;
			return;
		}

		_fd = fd;
		_valid = true;
		_ec.clear();
#endif
	}

} // namespace pylabhub::fileutil
