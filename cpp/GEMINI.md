# PROJECT CONTEXT & COMPLIANCE RULES

## 1. Build System Authority (CMake)
* **Primary System:** The project relies entirely on CMake for the build process.
* **Mandatory Check:** Any proposed changes to the codebase (C++, Python, etc.) MUST be cross-referenced with the existing `CMakeLists.txt` and CMake settings at the relevant directory level.
* **Goal:** You must ensure new code does not introduce linking errors, missing dependencies, or conflicts with defined targets.
* **Helper Locations:** Be aware that `cmake/` subdirectories contain essential helper functions and options. Reference these before suggesting custom build logic.

## 2. Documentation Synchronization
* **Principle:** Documentation is treated as part of the code.
* **Requirement:** When modifying logic or design patterns, you MUST review the corresponding `docs/` subdirectory.
* **Action:** If a code change contradicts or updates a design principle, explicitly suggest updates to the documentation to keep it consistent with the new implementation.

## 3. Scope & Exclusions
* **DO NOT SCAN:** `third_party/`
    * This folder contains upstream code. It is out of scope for analysis or modification unless explicitly instructed.
* **Focus Areas:** Focus entirely on our main source directories, `cmake/` configs, and `docs/`.

## 4. Git Commit Practices
* **Complex Commit Messages:** When composing commit messages, especially those with multiple lines or special characters, ALWAYS use a temporary file for the commit message. This avoids shell escaping issues and ensures the message is correctly preserved.
    * **Action:**
        1. Create a file (e.g., `.gemini_commit_message.txt`) with the desired message content.
        2. Use `git commit -F .gemini_commit_message.txt`.
        3. Ensure the temporary file name is added to `.gitignore`.
        4. Remove the temporary file after the commit is successful.