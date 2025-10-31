#pragma once
// JsonConfig.hpp - header-only JsonConfig implementation
// - Uses FileLock (FileLock.hpp) for cross-process locking
// - Defaults to NonBlocking lock mode and fails gracefully if lock cannot be acquired
// - Provides with_json_read / with_json_write helpers (return bool) to operate under lock
//
// Notes:
// - This header is intentionally self-contained and header-only. It therefore includes
//   some POSIX headers required by the atomic-write implementation (mkstemp, write, fsync).
// - If you prefer a lighter header, move atomic_write_json into JsonConfig.cpp and keep
//   the rest header-only.
// - Keep callbacks passed to with_json_* small and fast. Avoid calling save() from inside
//   a with_json_write callback (risk of deadlock due to lock ordering).

#include "FileLock.hpp"               // must be available and compiled separately
#include <nlohmann/json.hpp>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <fstream>
#include <sstream>
#include <optional>
#include <iostream>
#include <system_error>
#include <vector>
#include <memory>
#include <algorithm>
#include <type_traits>

#if !defined(_WIN32)
  // POSIX-only headers needed for mkstemp/write/fsync/rename/...
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <cstring>
#else
  // Windows-only headers needed for CreateFileW/WriteFile/FlushFileBuffers/ReplaceFileW/...
#include <windows.h>
#endif

namespace pylabhub::fileutil {

	using json = nlohmann::json;

	class JsonConfig {
	public:
		JsonConfig() noexcept = default;
		explicit JsonConfig(const std::filesystem::path& configFile) { init(configFile, false); }
		~JsonConfig() = default;

		JsonConfig(const JsonConfig&) = delete;
		JsonConfig& operator=(const JsonConfig&) = delete;
		JsonConfig(JsonConfig&&) noexcept = default;
		JsonConfig& operator=(JsonConfig&&) noexcept = default;

		// initialize config (set path). If createIfMissing==true tries to create the file (non-blocking).
		bool init(const std::filesystem::path& configFile, bool createIfMissing = false);

		// persist to disk (non-blocking lock). Return false if lock not acquired or IO error.
		bool save() noexcept;

		// reload from disk (non-blocking lock). Return false if lock not acquired or IO error.
		bool reload() noexcept;

		// Return a copy of the in-memory json (thread-safe).
		json as_json() const noexcept;

		/// Atomically replace both the in-memory JSON and the on-disk file.
		/// 
		/// Attempts to acquire a non-blocking file lock and write `newData` to disk.
		/// If successful, updates the in-memory JSON to match `newData`.
		/// Returns `true` on success, `false` if the lock is busy or any I/O error occurs.
		/// 
		/// Unlike `save()`, which only writes existing data, `replace()` also updates memory.
		/// Memory is untouched if the writing fails.
		bool replace(const json& newData) noexcept;

		// Simple template helpers that operate on the JSON without copying whole object:
		// - with_json_read(cb): cb(const json&) executes under shared (read) lock. Returns true on success.
		// - with_json_write(cb): cb(json&) executes under exclusive (write) lock. Returns true on success.
		//
		// Both accept any callable type F. They return false on error (e.g., not initialized or exception).
		template<typename F>
		bool with_json_read(F&& cb) const noexcept;

		template<typename F>
		bool with_json_write(F&& cb);

		// Convenience small helpers (copy-based) for simple reads/writes
		template<typename T>
		std::optional<T> get_optional(const std::string& path) const noexcept;

		template<typename T>
		T get_or(const std::string& path, const T& def) const noexcept;

		template<typename T>
		T get(const std::string& path) const;

		template<typename T>
		void set(const std::string& path, T&& value);

		bool remove(const std::string& path) noexcept;
		bool has(const std::string& path) const noexcept;

	private:
		struct Impl {
			std::filesystem::path configPath;
			json data;
			mutable std::shared_mutex rwMutex; // protects data
		};

		std::unique_ptr<Impl> _impl;
		mutable std::mutex _initMutex; // guards _impl initialization and life-time

		// atomic write helper (POSIX durable, Windows fallback)
		static void atomic_write_json(const std::filesystem::path& target, const json& j);
	};

	// ---------------- Implementation (inline) ----------------

	inline bool JsonConfig::init(const std::filesystem::path& configFile, bool createIfMissing) {
		std::lock_guard<std::mutex> g(_initMutex);
		if (!_impl) _impl = std::make_unique<Impl>();
		_impl->configPath = configFile;

		if (createIfMissing) {
			std::error_code ec;
			if (!std::filesystem::exists(configFile, ec)) {
				// Non-blocking: fail fast if someone else holds the lock
				FileLock flock(configFile, LockMode::NonBlocking);
				if (!flock.valid()) {
					auto e = flock.error_code();
					std::cerr << "JsonConfig::init: cannot create file (lock): " << configFile
						<< " code=" << e.value() << " msg=\"" << e.message() << "\"\n";
					return false;
				}
				try {
					atomic_write_json(configFile, json::object());
				}
				catch (const std::exception& ex) {
					std::cerr << "JsonConfig::init: failed to create file: " << ex.what() << "\n";
					return false;
				}
				catch (...) {
					std::cerr << "JsonConfig::init: unknown error creating file\n";
					return false;
				}
			}
		}

		return reload();
	}

	inline bool JsonConfig::save() noexcept {
		try {
			std::lock_guard<std::mutex> g(_initMutex);
			if (!_impl) return false;

			// Non-blocking lock by default per your requirement
			FileLock flock(_impl->configPath, LockMode::NonBlocking);
			if (!flock.valid()) {
				auto ec = flock.error_code();
				std::cerr << "JsonConfig::save: failed to acquire lock for " << _impl->configPath
					<< " code=" << ec.value() << " msg=\"" << ec.message() << "\"\n";
				return false;
			}

			// copy under read lock, then write outside of the in-memory lock
			json toWrite;
			{
				std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
				toWrite = _impl->data;
			}

			atomic_write_json(_impl->configPath, toWrite);
			return true;
		}
		catch (const std::exception& e) {
			std::cerr << "JsonConfig::save: exception: " << e.what() << "\n";
			return false;
		}
		catch (...) {
			std::cerr << "JsonConfig::save: unknown exception\n";
			return false;
		}
	}

	inline bool JsonConfig::reload() noexcept {
		try {
			std::lock_guard<std::mutex> g(_initMutex);
			if (!_impl) return false;

			FileLock flock(_impl->configPath, LockMode::NonBlocking);
			if (!flock.valid()) {
				auto ec = flock.error_code();
				std::cerr << "JsonConfig::reload: failed to acquire lock for " << _impl->configPath
					<< " code=" << ec.value() << " msg=\"" << ec.message() << "\"\n";
				return false;
			}

			std::ifstream in(_impl->configPath);
			if (!in.is_open()) {
				std::cerr << "JsonConfig::reload: cannot open file: " << _impl->configPath << "\n";
				return false;
			}

			json newdata;
			in >> newdata;
			if (!in && !in.eof()) {
				std::cerr << "JsonConfig::reload: parse/read error for " << _impl->configPath << "\n";
				return false;
			}

			{
				std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
				_impl->data = std::move(newdata);
			}
			return true;
		}
		catch (const std::exception& e) {
			std::cerr << "JsonConfig::reload: exception: " << e.what() << "\n";
			return false;
		}
		catch (...) {
			std::cerr << "JsonConfig::reload: unknown exception\n";
			return false;
		}
	}

	inline bool JsonConfig::replace(const json& newData) noexcept {
		try {
			// Protect _impl lifetime
			std::lock_guard<std::mutex> g(_initMutex);
			if (!_impl) _impl = std::make_unique<Impl>();

			// Acquire non-blocking cross-process lock; fail fast if busy
			FileLock flock(_impl->configPath, LockMode::NonBlocking);
			if (!flock.valid()) {
				auto ec = flock.error_code();
				std::cerr << "JsonConfig::replace: failed to acquire lock for " << _impl->configPath
					<< " code=" << ec.value() << " msg=\"" << ec.message() << "\"\n";
				return false;
			}

			// 1) Persist newData to disk atomically (may throw).
			atomic_write_json(_impl->configPath, newData);

			// 2) Update in-memory data under write lock
			{
				std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
				_impl->data = newData;
			}

			return true;
		}
		catch (const std::exception& e) {
			std::cerr << "JsonConfig::replace: exception: " << e.what() << "\n";
			return false;
		}
		catch (...) {
			std::cerr << "JsonConfig::replace: unknown exception\n";
			return false;
		}
	}

	inline json JsonConfig::as_json() const noexcept {
		try {
			std::lock_guard<std::mutex> g(_initMutex);
			if (!_impl) return json::object();
			std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
			return _impl->data;
		}
		catch (...) {
			return json::object();
		}
	}

	// ---------------- with_json helpers (simple bool-returning versions) ----------------
	// do not call save() from inside with_json_write callback (risk of deadlock)
	template<typename F>
	inline bool JsonConfig::with_json_read(F&& cb) const noexcept {
		try {
			std::lock_guard<std::mutex> g(_initMutex);
			if (!_impl) return false;
			std::shared_lock<std::shared_mutex> r(_impl->rwMutex);
			std::forward<F>(cb)(_impl->data);
			return true;
		}
		catch (...) {
			return false;
		}
	}

	template<typename F>
	inline bool JsonConfig::with_json_write(F&& cb) {
		try {
			std::lock_guard<std::mutex> g(_initMutex);
			if (!_impl) _impl = std::make_unique<Impl>();
			std::unique_lock<std::shared_mutex> w(_impl->rwMutex);
			std::forward<F>(cb)(_impl->data);
			return true;
		}
		catch (...) {
			return false;
		}
	}

	// ---------------- small copy-based helpers ----------------

	template<typename T>
	inline std::optional<T> JsonConfig::get_optional(const std::string& path) const noexcept {
		try {
			json j = as_json(); // copy
			size_t pos = 0;
			const json* cur = &j;
			while (pos < path.size()) {
				auto dot = path.find('.', pos);
				std::string key = (dot == std::string::npos) ? path.substr(pos) : path.substr(pos, dot - pos);
				if (!cur->contains(key)) return std::nullopt;
				cur = &(*cur)[key];
				if (dot == std::string::npos) break;
				pos = dot + 1;
			}
			return cur->get<T>();
		}
		catch (...) {
			return std::nullopt;
		}
	}

	template<typename T>
	inline T JsonConfig::get_or(const std::string& path, const T& def) const noexcept {
		auto v = get_optional<T>(path);
		if (v) return *v;
		return def;
	}

	template<typename T>
	inline T JsonConfig::get(const std::string& path) const {
		auto v = get_optional<T>(path);
		if (!v) throw std::runtime_error("JsonConfig::get: key not found or wrong type: " + path);
		return *v;
	}

	template<typename T>
	inline void JsonConfig::set(const std::string& path, T&& value) {
		std::lock_guard<std::mutex> g(_initMutex);
		if (!_impl) _impl = std::make_unique<Impl>();
		std::unique_lock<std::shared_mutex> w(_impl->rwMutex);

		size_t pos = 0;
		json* cur = &_impl->data;
		while (true) {
			auto dot = path.find('.', pos);
			std::string key = (dot == std::string::npos) ? path.substr(pos) : path.substr(pos, dot - pos);
			if (dot == std::string::npos) {
				(*cur)[key] = std::forward<T>(value);
				break;
			}
			else {
				if (!cur->contains(key) || !(*cur)[key].is_object()) {
					(*cur)[key] = json::object();
				}
				cur = &(*cur)[key];
				pos = dot + 1;
			}
		}
	}

	inline bool JsonConfig::remove(const std::string& path) noexcept {
		try {
			std::lock_guard<std::mutex> g(_initMutex);
			if (!_impl) return false;
			std::unique_lock<std::shared_mutex> w(_impl->rwMutex);

			size_t pos = 0;
			json* cur = &_impl->data;
			std::vector<std::string> keys;
			while (pos < path.size()) {
				auto dot = path.find('.', pos);
				std::string key = (dot == std::string::npos) ? path.substr(pos) : path.substr(pos, dot - pos);
				keys.push_back(key);
				if (dot == std::string::npos) break;
				pos = dot + 1;
				if (!cur->contains(key) || !(*cur)[key].is_object()) {
					return false;
				}
				cur = &(*cur)[key];
			}
			// walk to parent
			cur = &_impl->data;
			for (size_t i = 0; i + 1 < keys.size(); ++i) cur = &(*cur)[keys[i]];
			return cur->erase(keys.back()) > 0;
		}
		catch (...) {
			return false;
		}
	}

	inline bool JsonConfig::has(const std::string& path) const noexcept {
		try {
			json j = as_json();
			size_t pos = 0;
			const json* cur = &j;
			while (pos < path.size()) {
				auto dot = path.find('.', pos);
				std::string key = (dot == std::string::npos) ? path.substr(pos) : path.substr(pos, dot - pos);
				if (!cur->contains(key)) return false;
				cur = &(*cur)[key];
				if (dot == std::string::npos) break;
				pos = dot + 1;
			}
			return true;
		}
		catch (...) {
			return false;
		}
	}

	// ---------------- atomic_write_json implementation ----------------

	inline void JsonConfig::atomic_write_json(const std::filesystem::path& target, const json& j) {

#if defined(_WIN32)
		// Windows: create temp file in same directory and atomically replace using ReplaceFileW
		// Convert target path to long-path
		std::wstring target_long = win32_to_long_path(target);

		// Determine directory in long-path form
		std::filesystem::path parent = target.parent_path();
		if (parent.empty()) parent = L".";
		std::wstring parent_long = win32_to_long_path(parent);

		// Create a temp filename in same directory: <targetfilename>.tmp<suffix>
		std::wstring filename = target.filename().wstring();
		std::wstring tempName = filename + L".tmp" + win32_make_unique_suffix();

		// Build full tmp path in long form
		std::wstring tmp_full_long = parent_long;
		// Ensure directory path ends with backslash for concatenation
		if (tmp_full_long.size() && tmp_full_long.back() != L'\\' && tmp_full_long.back() != L'/') {
			tmp_full_long += L"\\";
		}
		tmp_full_long += tempName;

		// Create temp file (no sharing)
		HANDLE h = CreateFileW(
			tmp_full_long.c_str(),
			GENERIC_WRITE,
			0, // no sharing
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);

		if (h == INVALID_HANDLE_VALUE) {
			DWORD err = GetLastError();
			std::ostringstream os;
			os << "atomic_write_json: CreateFileW(temp) failed: " << err;
			throw std::runtime_error(os.str());
		}

		// Write JSON bytes
		std::string outstr = j.dump(4);
		DWORD written = 0;
		BOOL ok = WriteFile(h, outstr.data(), static_cast<DWORD>(outstr.size()), &written, nullptr);
		if (!ok || written != outstr.size()) {
			DWORD err = GetLastError();
			// best-effort cleanup
			FlushFileBuffers(h);
			CloseHandle(h);
			DeleteFileW(tmp_full_long.c_str());
			std::ostringstream os;
			os << "atomic_write_json: WriteFile failed: " << err;
			throw std::runtime_error(os.str());
		}

		// Flush to disk
		if (!FlushFileBuffers(h)) {
			DWORD err = GetLastError();
			CloseHandle(h);
			DeleteFileW(tmp_full_long.c_str());
			std::ostringstream os;
			os << "atomic_write_json: FlushFileBuffers failed: " << err;
			throw std::runtime_error(os.str());
		}

		// Close the temp file handle before ReplaceFileW
		CloseHandle(h);

		// Replace target atomically. Use REPLACEFILE_WRITE_THROUGH for stronger durability.
		BOOL replaced = ReplaceFileW(
			target_long.c_str(),   // lpReplacedFileName
			tmp_full_long.c_str(), // lpReplacementFileName
			nullptr,               // lpBackupFileName (optional)
			REPLACEFILE_WRITE_THROUGH,
			nullptr,
			nullptr
		);

		if (!replaced) {
			DWORD err = GetLastError();
			// Attempt cleanup
			DeleteFileW(tmp_full_long.c_str());
			std::ostringstream os;
			os << "atomic_write_json: ReplaceFileW failed: " << err;
			throw std::runtime_error(os.str());
		}

		// ReplaceFileW should have moved tmp into place; try to delete any leftover (no-op if gone)
		DeleteFileW(tmp_full_long.c_str());

#else // POSIX implementation

		namespace fs = std::filesystem;
		std::string dir = target.parent_path().string();
		if (dir.empty()) dir = ".";

		std::error_code ec;
		fs::create_directories(target.parent_path(), ec);
		if (ec) throw std::runtime_error("atomic_write_json: create_directories failed: " + ec.message());

		std::string filename = target.filename().string();
		std::string tmpl = dir + "/" + filename + ".tmp.XXXXXX";
		std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
		tmpl_buf.push_back('\0');

		int fd = mkstemp(tmpl_buf.data());
		if (fd == -1) {
			int err = errno;
			std::ostringstream os;
			os << "atomic_write_json: mkstemp failed: " << std::strerror(err);
			throw std::runtime_error(os.str());
		}

		std::string tmp_path = tmpl_buf.data();
		bool tmp_unlinked = false;

		try {
			std::string outstr = j.dump(4);
			const char* buf = outstr.data();
			size_t toWrite = outstr.size();
			size_t written = 0;
			while (toWrite > 0) {
				ssize_t w = ::write(fd, buf + written, toWrite);
				if (w < 0) {
					int err = errno;
					::close(fd);
					::unlink(tmp_path.c_str());
					std::ostringstream os;
					os << "atomic_write_json: write failed: " << std::strerror(err);
					throw std::runtime_error(os.str());
				}
				written += static_cast<size_t>(w);
				toWrite -= static_cast<size_t>(w);
			}

			if (::fsync(fd) != 0) {
				int err = errno;
				::close(fd);
				::unlink(tmp_path.c_str());
				std::ostringstream os;
				os << "atomic_write_json: fsync(file) failed: " << std::strerror(err);
				throw std::runtime_error(os.str());
			}

			struct stat st;
			if (stat(target.c_str(), &st) == 0) {
				if (fchmod(fd, st.st_mode) != 0) {
					int err = errno;
					::close(fd);
					::unlink(tmp_path.c_str());
					std::ostringstream os;
					os << "atomic_write_json: fchmod failed: " << std::strerror(err);
					throw std::runtime_error(os.str());
				}
			}

			if (::close(fd) != 0) {
				int err = errno;
				::unlink(tmp_path.c_str());
				std::ostringstream os;
				os << "atomic_write_json: close failed: " << std::strerror(err);
				throw std::runtime_error(os.str());
			}
			fd = -1;

			if (std::rename(tmp_path.c_str(), target.c_str()) != 0) {
				int err = errno;
				::unlink(tmp_path.c_str());
				std::ostringstream os;
				os << "atomic_write_json: rename failed: " << std::strerror(err);
				throw std::runtime_error(os.str());
			}
			tmp_unlinked = true;

			int dfd = ::open(dir.c_str(), O_DIRECTORY | O_RDONLY);
			if (dfd >= 0) {
				if (::fsync(dfd) != 0) {
					int err = errno;
					::close(dfd);
					std::ostringstream os;
					os << "atomic_write_json: fsync(dir) failed: " << std::strerror(err);
					throw std::runtime_error(os.str());
				}
				::close(dfd);
			}
			else {
				int err = errno;
				std::ostringstream os;
				os << "atomic_write_json: open(dir) failed for fsync: " << std::strerror(err);
				throw std::runtime_error(os.str());
			}
		}
		catch (...) {
			if (!tmp_unlinked) {
				::unlink(tmp_path.c_str());
			}
			throw;
		}
#endif
	}

} // namespace pylabhub::fileutil
