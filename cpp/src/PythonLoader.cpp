// PythonLoader.cpp
//
// Dynamic-only Python loader. Does NOT require Python headers at compile time.
// - Loads python3*.dll dynamically from venv / supplied path / local resources/venv
// - Emulates key PyConfig behavior by setting environment variables (PYTHONHOME, PYTHONUTF8, PYTHONNOUSERSITE, ...)
 // and running a short bootstrap via PyRun_SimpleString
// - Persists venv info and cleanup callable to a JSON file next to the XOP
// - Refuses re-init if interpreter already initialized
// - Exposes PySetPython, PySetCleanupCallable, PyReInit, PyIsInitialized, PyExec, PyCleanup
//
// Note: This implementation uses the minimal, widely-available CPython C-API symbols:
 // Py_Initialize, Py_IsInitialized, Py_FinalizeEx (if present) / Py_Finalize, PyGILState_Ensure/Release, PyRun_SimpleString.
// It is intentionally conservative and portable across Python 3.x builds on Windows.

#include "XOPStandardHeaders.h"
#include "IgorErrors.h"
#include "PythonLoader.hpp"

#include <windows.h>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <mutex>



#ifndef MAX_LONG_NAME
#define MAX_LONG_NAME 1024
#endif

namespace pyportal {

	// JSON config filename (stored next to the XOP)
	static const wchar_t* kConfigFileName = L"python_config.json";

	// Global state
	static std::mutex g_loaderMutex;
	static std::wstring g_xopFolderW;      // folder where XOP DLL lives
	static std::string  g_userPathUtf8;    // user supplied path (utf-8)
	static std::wstring g_venvPathW;       // resolved venv folder (wide)
	static std::string  g_cleanupCallable; // "module:function"
	static HMODULE      g_pyDll = NULL;
	static bool         g_initialized = false;

	// Function pointers resolved from python DLL
	typedef void (*py_void_fn)(void);
	typedef int  (*py_int_fn)(void);
	typedef void* (*py_gil_fn)(void);
	typedef void (*py_gil_rel_fn)(void*);
	typedef int  (*py_run_fn)(const char*);

	static py_void_fn p_Py_Initialize = NULL;
	static py_int_fn  p_Py_IsInitialized = NULL;
	static py_void_fn p_Py_Finalize = NULL;
	static py_void_fn p_Py_FinalizeEx = NULL; // some builds have this as int, but we'll call if present
	static py_gil_fn  p_PyGILState_Ensure = NULL;
	static py_gil_rel_fn p_PyGILState_Release = NULL;
	static py_run_fn  p_PyRun_SimpleString = NULL;

	// ---- Utility helpers -------------------------------------------------------

	void PostHistory(const std::string& s)
	{
		Handle h = WMNewHandle((BCInt)s.size());
		if (!h) return;
		memcpy(*h, s.data(), (size_t)s.size());
		HistoryInsert(*h, (long)GetHandleSize(h));
		WMDisposeHandle(h);
	}

	static void SetStatus(const std::string& s)
	{
		PostHistory(std::string("PyLoader: ") + s + "\n");
	}

	// UTF conversion helpers
	static std::wstring UTF8ToW(const std::string& s)
	{
		if (s.empty()) return std::wstring();
		int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
		std::wstring w(n, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
		return w;
	}
	static std::string WToUTF8(const std::wstring& w)
	{
		if (w.empty()) return std::string();
		int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
		std::string s(n, '\0');
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
		return s;
	}

	// Get folder containing the XOP DLL
	static std::wstring GetModuleFolder()
	{
		HMODULE h = NULL;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&GetModuleFolder), &h)) {
			return L"";
		}
		std::wstring buf(MAX_PATH, L'\0');
		DWORD n = GetModuleFileNameW(h, &buf[0], (DWORD)buf.size());
		if (n == 0) return L"";
		buf.resize(n);
		size_t pos = buf.find_last_of(L"\\/");
		if (pos == std::wstring::npos) return L"";
		return buf.substr(0, pos);
	}

	// Read entire file to string
	static bool ReadFileToString(const std::wstring& pathW, std::string& out)
	{
		std::ifstream ifs(WToUTF8(pathW), std::ios::binary);
		if (!ifs) return false;
		std::ostringstream oss; oss << ifs.rdbuf();
		out = oss.str();
		return true;
	}

	// Write string to file (overwrite)
	static bool WriteStringToFile(const std::wstring& pathW, const std::string& s)
	{
		std::ofstream ofs(WToUTF8(pathW), std::ios::binary | std::ios::trunc);
		if (!ofs) return false;
		ofs.write(s.data(), (std::streamsize)s.size());
		return true;
	}

	// JSON parse (very small; tolerant): keys: "venv_path" and "cleanup_callable"
	static void ParseJsonConfig(const std::string& json, std::string& venvOut, std::string& cleanupOut)
	{
		auto findVal = [&](const std::string& key)->std::string {
			size_t k = json.find("\"" + key + "\"");
			if (k == std::string::npos) return std::string();
			size_t colon = json.find(':', k);
			if (colon == std::string::npos) return std::string();
			size_t q1 = json.find('"', colon + 1);
			if (q1 == std::string::npos) return std::string();
			size_t q2 = json.find('"', q1 + 1);
			if (q2 == std::string::npos) return std::string();
			return json.substr(q1 + 1, q2 - q1 - 1);
			};
		venvOut = findVal("venv_path");
		cleanupOut = findVal("cleanup_callable");
	}

	// Compose config JSON from current state and write to disk next to XOP
	static void SaveConfig()
	{
		std::wstring cfgPath = g_xopFolderW;
		cfgPath += L"\\";
		cfgPath += kConfigFileName;

		std::ostringstream oss;
		oss << "{\n";
		if (!WToUTF8(g_venvPathW).empty()) {
			oss << "  \"venv_path\": \"" << WToUTF8(g_venvPathW) << "\"";
			if (!g_cleanupCallable.empty()) oss << ",\n"; else oss << "\n";
		}
		if (!g_cleanupCallable.empty()) {
			oss << "  \"cleanup_callable\": \"" << g_cleanupCallable << "\"\n";
		}
		oss << "}\n";
		WriteStringToFile(cfgPath, oss.str());
	}

	// Try to find python DLL in given folder (windows). Return full path UTF-8 if found.
	static std::string FindPythonDllInFolder(const std::wstring& folderW)
	{
		std::vector<std::wstring> names = { L"python3.13.dll", L"python3.dll", L"python.dll" };
		for (auto& n : names) {
			std::wstring full = folderW;
			if (!full.empty() && full.back() != L'\\') full += L'\\';
			full += n;
			DWORD attr = GetFileAttributesW(full.c_str());
			if (attr != INVALID_FILE_ATTRIBUTES) return WToUTF8(full);
		}
		return std::string();
	}

	// Try dynamic load from candidate folders (also tries plain names on PATH)
	static bool LoadPythonDllFromCandidates(const std::vector<std::wstring>& candidates, std::string& loadedOut)
	{
		// Prefer safer DLL search
		SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);

		for (auto& f : candidates) {
			if (f.empty()) continue;
			AddDllDirectory(f.c_str());
			std::string path = FindPythonDllInFolder(f);
			if (!path.empty()) {
				HMODULE h = LoadLibraryW(UTF8ToW(path).c_str());
				if (h) {
					g_pyDll = h;
					loadedOut = path;
					return true;
				}
				// try just filename
				size_t pos = path.find_last_of("\\/");
				std::string name = (pos == std::string::npos) ? path : path.substr(pos + 1);
				HMODULE h2 = LoadLibraryW(UTF8ToW(name).c_str());
				if (h2) { g_pyDll = h2; loadedOut = (WToUTF8(f) + "\\" + name); return true; }
			}
		}
		// fallback: try loading by common names on PATH
		const wchar_t* names[] = { L"python3.13.dll", L"python3.dll", L"python.dll" };
		for (auto n : names) {
			HMODULE h = LoadLibraryW(n);
			if (h) { g_pyDll = h; loadedOut = WToUTF8(std::wstring(n)); return true; }
		}
		return false;
	}

	// Resolve minimal symbols we need at runtime
	static bool ResolveCommonSymbols()
	{
		if (!g_pyDll) return false;
		p_Py_Initialize = (py_void_fn)GetProcAddress(g_pyDll, "Py_Initialize");
		p_Py_IsInitialized = (py_int_fn)GetProcAddress(g_pyDll, "Py_IsInitialized");
		p_Py_Finalize = (py_void_fn)GetProcAddress(g_pyDll, "Py_Finalize");
		p_Py_FinalizeEx = (py_void_fn)GetProcAddress(g_pyDll, "Py_FinalizeEx"); // may be NULL
		p_PyGILState_Ensure = (py_gil_fn)GetProcAddress(g_pyDll, "PyGILState_Ensure");
		p_PyGILState_Release = (py_gil_rel_fn)GetProcAddress(g_pyDll, "PyGILState_Release");
		p_PyRun_SimpleString = (py_run_fn)GetProcAddress(g_pyDll, "PyRun_SimpleString");

		return (p_Py_Initialize && p_Py_IsInitialized && p_PyRun_SimpleString);
	}

	// Run cleanup callable (module:function) in interpreter (best-effort)
	static void RunCleanupCallable()
	{
		if (g_cleanupCallable.empty()) return;
		if (!p_PyGILState_Ensure || !p_PyRun_SimpleString) return;

		// Build driver to import module and call function; swallow exceptions
		size_t colon = g_cleanupCallable.find(':');
		std::string driver;
		if (colon == std::string::npos) {
			// treat whole string as a single callable name in globals
			driver =
				"import traceback\n"
				"try:\n"
				"    f = globals().get('" + g_cleanupCallable + "') or locals().get('" + g_cleanupCallable + "')\n"
				"    if f: f()\n"
				"except Exception:\n"
				"    traceback.print_exc()\n";
		}
		else {
			std::string mod = g_cleanupCallable.substr(0, colon);
			std::string fn = g_cleanupCallable.substr(colon + 1);
			driver =
				"import traceback\n"
				"try:\n"
				"    m = __import__(\"" + mod + "\", fromlist=[\"" + fn + "\"])\n"
				"    f = getattr(m, \"" + fn + "\", None)\n"
				"    if f: f()\n"
				"except Exception:\n"
				"    traceback.print_exc()\n";
		}

		void* gil = p_PyGILState_Ensure();
		p_PyRun_SimpleString(driver.c_str());
		if (p_PyGILState_Release) p_PyGILState_Release(gil);
	}

	// Finalize & unload python DLL (used by PyCleanup and by reinit)
	static int FinalizeAndUnload()
	{
		if (!g_pyDll) { g_initialized = false; return 0; }

		// Run user cleanup callable first
		RunCleanupCallable();

		// Prefer Py_FinalizeEx if available (may return int) else Py_Finalize
		if (p_Py_FinalizeEx) {
			// many builds have Py_FinalizeEx signature int Py_FinalizeEx(void); call via GetProcAddress if symbol exists
			typedef int (*Py_FinalizeEx_t)(void);
			Py_FinalizeEx_t pFE = (Py_FinalizeEx_t)GetProcAddress(g_pyDll, "Py_FinalizeEx");
			if (pFE) {
				pFE();
			}
			else if (p_Py_Finalize) {
				p_Py_Finalize();
			}
		}
		else if (p_Py_Finalize) {
			p_Py_Finalize();
		}
		else {
			PostHistory("PyLoader: finalize symbol not available; skipping finalize");
		}

		FreeLibrary(g_pyDll);
		g_pyDll = NULL;

		// clear pointers
		p_Py_Initialize = NULL;
		p_Py_IsInitialized = NULL;
		p_Py_Finalize = NULL;
		p_Py_FinalizeEx = NULL;
		p_PyGILState_Ensure = NULL;
		p_PyGILState_Release = NULL;
		p_PyRun_SimpleString = NULL;

		g_initialized = false;
		SetStatus("Python finalized and DLL unloaded");
		return 0;
	}

	// ---------------- Public API implementations ----------------

	int PyLoader_init(void)
	{
		std::lock_guard<std::mutex> lk(g_loaderMutex);

		g_xopFolderW = GetModuleFolder();
		if (g_xopFolderW.empty()) {
			SetStatus("cannot determine XOP folder");
		}

		// Load config if present
		std::wstring cfgW = g_xopFolderW + L"\\" + kConfigFileName;
		std::string cfg;
		if (ReadFileToString(cfgW, cfg)) {
			std::string venv, cleanup;
			ParseJsonConfig(cfg, venv, cleanup);
			if (!venv.empty()) g_venvPathW = UTF8ToW(venv);
			if (!cleanup.empty()) g_cleanupCallable = cleanup;
		}

		SetStatus("loader initialized (python uninitialized)");
		return 0;
	}

	int PyLoader_cleanup(void)
	{
		std::lock_guard<std::mutex> lk(g_loaderMutex);
		FinalizeAndUnload();
		SetStatus("loader cleanup complete");
		return 0;
	}

	int PySetPython(const char* pathUtf8)
	{
		if (!pathUtf8) return WMparamErr; // invalid param
		std::lock_guard<std::mutex> lk(g_loaderMutex);

		std::string s(pathUtf8);
		if (s.empty()) return WMparamErr;

		g_userPathUtf8 = s;
		g_venvPathW = UTF8ToW(s);

		// persist to JSON
		SaveConfig();

		SetStatus(std::string("PySetPython: stored path ") + s);
		return 0;
	}

	int PySetCleanupCallable(const char* callableUtf8)
	{
		if (!callableUtf8) return WMparamErr;
		std::lock_guard<std::mutex> lk(g_loaderMutex);
		g_cleanupCallable = callableUtf8;
		SaveConfig();
		SetStatus(std::string("PySetCleanupCallable: ") + g_cleanupCallable);
		return 0;
	}

	int PyReInit(const char* pathUtf8)
	{
		std::lock_guard<std::mutex> lk(g_loaderMutex);

		// safety: refuse if already initialized
		if (g_initialized || (g_pyDll && p_Py_IsInitialized && p_Py_IsInitialized())) {
			SetStatus("PyReInit: interpreter already initialized; call PyCleanup first");
			return XOP_RECURSION_ATTEMPTED; // meaningful Igor error
		}

		// if user passed a path, store it and update JSON; else use persisted
		if (pathUtf8 && pathUtf8[0] != '\0') {
			g_userPathUtf8 = std::string(pathUtf8);
			g_venvPathW = UTF8ToW(g_userPathUtf8);
			SaveConfig();
			PostHistory(std::string("PyReInit: updated config with path: ") + g_userPathUtf8 + "\n");
		}

		// Build candidate folders: venv root, venv\Scripts, XOP\resources\venv, XOP folder
		std::vector<std::wstring> candidates;
		if (!g_venvPathW.empty()) {
			candidates.push_back(g_venvPathW);
			candidates.push_back(g_venvPathW + L"\\Scripts");
			candidates.push_back(g_venvPathW + L"\\Lib");
		}
		else {
			// check for local resources/venv
			std::wstring localV = g_xopFolderW + L"\\resources\\venv";
			DWORD a = GetFileAttributesW((localV + L"\\Scripts\\python.exe").c_str());
			if (a != INVALID_FILE_ATTRIBUTES) {
				candidates.push_back(localV);
				candidates.push_back(localV + L"\\Scripts");
				candidates.push_back(localV + L"\\Lib");
				g_venvPathW = localV;
			}
		}
		// user gave an exact python.exe path? allow parent folder
		if (!g_userPathUtf8.empty()) {
			std::wstring up = UTF8ToW(g_userPathUtf8);
			size_t pos = up.find_last_of(L"\\/");
			if (pos != std::wstring::npos) candidates.push_back(up.substr(0, pos));
			else candidates.push_back(up);
		}
		candidates.push_back(g_xopFolderW);

		// unique
		std::vector<std::wstring> uniq;
		for (auto& c : candidates) if (!c.empty() && std::find(uniq.begin(), uniq.end(), c) == uniq.end()) uniq.push_back(c);

		std::string loadedDll;
		if (!LoadPythonDllFromCandidates(uniq, loadedDll)) {
			SetStatus("PyReInit: failed to find/load python DLL");
			return XOP_LINK_FAILED;
		}

		// resolve symbols
		if (!ResolveCommonSymbols()) {
			if (g_pyDll) { FreeLibrary(g_pyDll); g_pyDll = NULL; }
			SetStatus("PyReInit: required symbols not found in python DLL");
			return XOP_LINK_FAILED;
		}

		// Emulate PyConfig by setting environment + program name before initialization
		if (!g_venvPathW.empty()) {
			SetEnvironmentVariableW(L"PYTHONHOME", g_venvPathW.c_str());
		}
		SetEnvironmentVariableA("PYTHONNOUSERSITE", "1");
		SetEnvironmentVariableA("PYTHONUTF8", "1");
		SetEnvironmentVariableA("PYTHONDONTWRITEBYTECODE", "1");

		// Optionally set program name (Py_SetProgramName) if available
		typedef void (*tp_Py_SetProgramName)(wchar_t*);
		tp_Py_SetProgramName pSetProg = (tp_Py_SetProgramName)GetProcAddress(g_pyDll, "Py_SetProgramName");
		if (pSetProg) {
			std::wstring prog = g_xopFolderW + L"\\QLabPyPortal";
			pSetProg(const_cast<wchar_t*>(prog.c_str()));
		}

		// Initialize interpreter
		if (!p_Py_Initialize) {
			SetStatus("PyReInit: Py_Initialize not available in DLL");
			FreeLibrary(g_pyDll); g_pyDll = NULL;
			return XOP_LINK_FAILED;
		}

		p_Py_Initialize();

		if (!(p_Py_IsInitialized && p_Py_IsInitialized())) {
			SetStatus("PyReInit: Py_Initialize failed to report initialized");
			if (g_pyDll) { FreeLibrary(g_pyDll); g_pyDll = NULL; }
			return kLibraryFailedToInitialize; // IgorErrors.h code
		}

		// Bootstrap: ensure venv site-packages is on sys.path and try to import xop_bootstrap
		if (p_PyRun_SimpleString) {
			std::string boot;
			if (!g_venvPathW.empty()) {
				boot += "import sys, os\n";
				boot += "sp = os.path.join(r'" + WToUTF8(g_venvPathW) + "', 'Lib', 'site-packages')\n";
				boot += "if os.path.isdir(sp) and sp not in sys.path: sys.path.insert(0, sp)\n";
			}
			boot += "try:\n";
			boot += "    import xop_bootstrap\n";
			boot += "    if hasattr(xop_bootstrap, 'configure'): xop_bootstrap.configure()\n";
			boot += "except Exception:\n";
			boot += "    pass\n";
			p_PyRun_SimpleString(boot.c_str());
		}

		g_initialized = true;
		SetStatus(std::string("PyReInit: Python initialized from DLL: ") + loadedDll);
		return 0;
	}

	int PyIsInitialized(void)
	{
		std::lock_guard<std::mutex> lk(g_loaderMutex);
		return g_initialized ? 1 : 0;
	}

	int PyExec(const char* igorStringVarName)
	{
		if (!igorStringVarName) return WMparamErr;
		std::lock_guard<std::mutex> lk(g_loaderMutex);

		if (!g_initialized) {
			SetStatus("PyExec: Python not initialized; call PyReInit first");
			return kLibraryFailedToInitialize;
		}

		// Fetch Igor string variable content
		Handle h = NULL;
		int ferr = FetchStringDataUsingVarName(igorStringVarName, &h);
		if (ferr != 0 || !h) {
			SetStatus(std::string("PyExec: failed to fetch Igor string variable: ") + igorStringVarName);
			return WMparamErr;
		}
		BCInt hsize = WMGetHandleSize(h);
		if (hsize <= 0) { WMDisposeHandle(h); SetStatus("PyExec: empty Igor string"); return WMparamErr; }
		const BCInt cap = 10 * 1024 * 1024;
		if (hsize > cap) hsize = cap;
		std::vector<char> buf((size_t)hsize + 1);
		int r = GetCStringFromHandle(h, buf.data(), (int)buf.size());
		WMDisposeHandle(h);
		if (r != 0) { SetStatus("PyExec: GetCStringFromHandle failed"); return WMparamErr; }
		buf[(size_t)hsize] = '\0';

		// Execute under GIL if available
		void* gil = NULL;
		if (p_PyGILState_Ensure) gil = p_PyGILState_Ensure();
		int rc = -1;
		if (p_PyRun_SimpleString) rc = p_PyRun_SimpleString(buf.data());
		if (p_PyGILState_Release && gil) p_PyGILState_Release(gil);
		if (rc != 0) {
			SetStatus("PyExec: execution returned non-zero");
			return kGeneralException;
		}
		SetStatus("PyExec: executed successfully");
		return 0;
	}

	int PyCleanup(void)
	{
		std::lock_guard<std::mutex> lk(g_loaderMutex);
		int rc = FinalizeAndUnload();
		return rc == 0 ? 0 : kGeneralException;
	}

} // namespace pyportal