# PROJECT CONTEXT, COMPLIANCE & AGENT INSTRUCTIONS

This document contains mandatory operating procedures. You must adhere to these rules for every action, analysis, and code modification.

## 1. Scope & System Authority
* **Exclusions:** NEVER scan, analyze, or modify the `third_party/` directory unless explicitly instructed by the user. It contains upstream code and is strictly out of scope.
* **Build System (CMake):** CMake is the absolute authority for the build process.
    * **Action:** If you propose changes to C++, Python, or project structures, you MUST cross-reference the `CMakeLists.txt` at the relevant directory level.
    * **Helper Functions:** Always check `cmake/` subdirectories for existing helper functions or options before inventing custom build logic.

## 2. Strict C++ Coding & Architecture Standards
When generating or modifying C++ code, you MUST adhere to the following project-specific rules:
* **Mandatory Braces:** All C/C++ control flow statements (`if`, `for`, `while`) MUST use braces `{}`, even for single-statement bodies. Follow Allman brace style.
* **Variable Naming:** Variables must be >= 3 characters and semantically meaningful. Never use `i`, `x`, `tmp`, or `res` outside of trivial, single-line scopes.
* **ABI Stability (pImpl):** All public classes MUST use the pImpl idiom. NEVER place STL containers (`std::vector`, `std::string`), third-party types, or platform-specific handles in public headers.
* **Header Inclusions:** Do NOT include individual module headers directly. You MUST use layered umbrellas (e.g., `#include "plh_platform.hpp"`, `plh_base.hpp`, `plh_service.hpp`, `plh_datahub.hpp`).
* **Error Handling & `noexcept`:** * Use error codes/returns for hot paths (e.g., slot acquisition) and exceptions for public API contract violations.
    * Explicitly mark destructors and non-throwing simple accessors as `noexcept`. Do NOT mark functions that can throw.

## 3. Concurrency, Memory & Security Rules
* **Memory Ordering (ARM Compliance):** NEVER rely on default `seq_cst` memory ordering. You MUST use explicit memory barriers: `std::memory_order_acquire` for loads and `std::memory_order_release` for stores.
* **Struct Initialization:** Any struct used in hashing, checksums (e.g., BLAKE2b), or `memcmp` MUST be explicitly value-initialized (e.g., `T var{}`) to prevent padding bytes from causing non-deterministic outputs.
* **DataHub API Usage:** Always prefer the Layer 2 Transaction API (RAII guards like `with_write_transaction` or `with_typed_write<T>`) over the manual Layer 0/1 C API to ensure exception safety and automatic resource release.

## 4. Code Modification Tool Rules (`replace` tool)
* **Context Verification:** Before executing a replacement, read the target chunk of the file to ensure your `old_string` is **100% unique** within that file.
* **Indentation & Whitespace:** Your `old_string` and `new_string` MUST preserve the exact original indentation, whitespace, and surrounding brackets.
* **Step-by-Step Validation:** If performing multiple dependent changes in one file, re-read the file between steps to verify the previous replacement succeeded without side effects.
* **Comment Preservation:** Never delete or alter existing comments unless the specific code change renders them explicitly obsolete. When adding comments, explain *why*, not *what*.

## 5. Iterative Debugging Strategy
If a failure is logical, behavioral, or state-driven:
1. **Instrument:** Insert explicit debug/log statements using the project `LOGGER_INFO`/`LOGGER_DEBUG` macros to output key variables. Do NOT use `std::cout` or `printf`.
2. **Observe & Reason:** Run the build/tests. Use the debug data to isolate the failure, apply a targeted patch, and re-test.
3. **Clean Up:** Once fixed, you MUST remove all temporary debug/log statements introduced in Step 1.

## 6. Documentation Synchronization
* **Principle:** Code and documentation are a single source of truth.
* **Action:** Whenever you modify core logic, design patterns, or architectural behavior, you MUST review the corresponding `docs/` subdirectory.
* **Output:** Explicitly propose updates to the relevant Markdown/doc files to reflect the new implementation.

## 7. Safe Git Commit Practices
* **Shell Escaping Safety:** NEVER pass complex commit messages directly via inline shell arguments (`git commit -m "..."`).
* **Workflow:**
    1. Write the complete message to `.gemini_commit_message.txt`.
    2. Execute: `git commit -F .gemini_commit_message.txt`.
    3. Execute: `rm .gemini_commit_message.txt` upon success.