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
        3. Remove the temporary file after the commit is successful.

## 5. General Debugging Strategy for Problems in the Code
* **Strategy:** If an error is not directly related to syntax or semantics, but rather to logic, design, behavior, or interaction between components, the first step is to identify the most important variables, states, or conditions in the flow of the functions and code.
* **Action:** Insert debug code to generate clear and useful information, providing intermediate values or states of these key elements that could impact results and contribute to pinpointing the failure/error.
* **Attention to Detail:** Pay close attention to string formats, especially when a string parameter needs to be used as input to another function that might be strict in its format.
* **Iterative Fixing:** After obtaining debug information, use thorough reasoning to find possible locations for the fix. Apply the patch, build the code, and re-test. Use the debug information after patching to determine if the fix is effective. Repeat this process, always with debug output available.
* **Clean Up:** Only remove the debug code when the failures or errors are fully addressed.
## 6. Code Modification Rules
* **Principle:** Extreme care must be taken when modifying code to prevent unintended side effects and to ensure precision.
* **Replace Tool Precision:** When using the `replace` tool, the `new_string` MUST preserve any and all context from the `old_string` that is not part of the intended change.
    * **Action:**
        1. Before executing a `replace` command, mentally verify or re-read the file to ensure the `old_string` is unique or that `expected_replacements` is set correctly.
        2. Critically inspect the `new_string` to confirm it contains the exact same surrounding code and indentation as the `old_string`.
        3. For a sequence of dependent changes, re-read the file between steps to verify the previous step was successful and did not introduce errors.
* **Comment Preservation:** Preserve existing comments in the code unless they are explicitly obsolete, misleading, or contradictory due to the changes being implemented. When adding new code, add high-value comments sparingly, focusing on *why* something is done rather than *what* is done.
* **Goal:** To guarantee that edits are minimal, correct, and do not corrupt surrounding code.
