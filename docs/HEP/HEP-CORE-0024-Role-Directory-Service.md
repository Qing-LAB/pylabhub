# HEP-CORE-0024: Role Directory Service

| Property       | Value                                                                          |
|----------------|--------------------------------------------------------------------------------|
| **HEP**        | `HEP-CORE-0024`                                                                |
| **Title**      | Role Directory Service — Canonical Layout, Path Resolution, and CLI Helpers    |
| **Status**     | Phases 1-12 done (2026-03-12..2026-04-17); Phases 13-14 done (2026-04-17); Phases 15-22 in progress (binary unification) |
| **Created**    | 2026-03-12                                                                     |
| **Area**       | Public API (`pylabhub::utils`), All Role Binaries, Custom Role Development     |
| **Depends on** | HEP-CORE-0018 (Producer/Consumer Binaries), HEP-CORE-0015 (Processor Binary)  |

---

## 1. Motivation

Every pyLabHub role (producer, consumer, processor) uses the same on-disk directory
layout convention. This convention is currently **implicit and scattered**:

| Responsibility | Current location |
|----------------|-----------------|
| Create `logs/`, `run/`, `vault/`, `script/python/` | `do_init()` — duplicated in each binary's `main.cpp` |
| Find config file from a directory | `Config::from_directory()` — duplicated in each role's config class |
| Resolve `hub_dir` relative → absolute | Each config class independently |
| Find `hub.pubkey` from hub directory | Each config class independently |
| Resolve script entry point path | Each script host independently |
| Default vault file path | Ad-hoc in each `--keygen` block |
| CLI argument parsing (`--init`, `--config`, `--keygen`, etc.) | Per-binary `parse_args()` — triplicated |
| Password prompt (no-echo, env-var, confirm) | Per-binary keygen block — triplicated |

This creates three problems:

1. **No public contract**: A developer building a custom C++ role binary has no standard
   way to set up the expected directory layout or parse the standard CLI flags. They must
   invent their own conventions, producing inconsistent tooling.

2. **Missed security enforcement**: `vault/` directory permissions (0700), default keyfile
   location, and vault password handling (echo suppression, confirmation, env-var priority)
   are security-critical details that every role reimplements — and custom roles will get
   wrong.

3. **Maintenance burden**: Adding a 4th standard binary, changing the directory layout,
   or fixing a path-resolution bug requires editing three places. Any inconsistency is
   a latent defect.

**This HEP introduces two public headers** that formalise the directory layout as a named
service (`RoleDirectory`) and provide a standard CLI toolkit (`role_cli.hpp`), enabling
consistent custom role binary development with correct security properties by default.

---

## 2. Design Principles

1. **Explicit contract over convention**: The directory layout is codified in `RoleDirectory`,
   not inferred by ad-hoc string concatenation throughout the codebase.

2. **Security by default**: `vault/` is always created at 0700. Default keyfile paths always
   land inside `vault/`. Password handling always suppresses echo and checks the env var
   before prompting. Custom roles get this for free by using the public API.

3. **Separation of concerns**: `RoleDirectory` knows about filesystem layout and path
   resolution only — it does not parse config JSON, manage lifecycles, or start threads.
   Config classes (`ProducerConfig` etc.) remain the authority on role-specific config content
   and delegate to `RoleDirectory` for path work.

4. **Extensibility**: Custom role types can add their own subdirectories via `subdir(name)`
   without subclassing. The standard layout is a floor, not a ceiling.

5. **Public API stability**: Both headers are installed in `build/stage-*/include/utils/`
   and versioned with `pylabhub-utils`. Breaking changes require a HEP revision.

---

## 3. Directory Layout Contract

### 3.1 Standard Role Directory

```
<role_dir>/                         ← RoleDirectory::base()
  <role>.json                       ← config file  (e.g. producer.json)
  logs/                             ← RoleDirectory::logs()    — Logger sink
  run/                              ← RoleDirectory::run()     — PID, FileLock artefacts
  vault/                            ← RoleDirectory::vault()   — RoleVault files (0700)
  schemas/                          ← OPTIONAL — local schema cache (HEP-CORE-0034 §13)
    lab/sensors/temperature.raw.v1.json
  script/
    python/
      __init__.py                   ← default script entry point
    lua/                            ← (future scripting engine)
      __init__.lua
  hub.pubkey                        ← optional: local copy of hub public key
```

### 3.2 Script Entry Point Convention

The script entry point is always resolved as:

```
<resolved_script_path>/script/<type>/__init__.<ext>
```

Where:
- `resolved_script_path` = `script.path` from config, resolved relative to `<role_dir>`
  if not absolute. Default: `"."` → `<role_dir>`.
- `type` = `script.type` from config (`"python"`, `"lua"`, etc.).
- `ext` = type-specific (`py`, `lua`, etc.).

This means `script.path = "."` always means "the script lives inside this role directory
at `./script/python/__init__.py`", regardless of the current working directory at runtime.

### 3.3 Hub Directory Reference

A role config may reference a hub directory via `hub_dir` (or `in_hub_dir` / `out_hub_dir`
for processors). These paths may be relative (to `<role_dir>`) or absolute:

```
<role_dir>/producer.json: { "hub_dir": "../hub" }
```

`RoleDirectory::resolve_hub_dir("../hub")` → `/absolute/path/to/hub`

Hub resources are then found at canonical locations within the resolved hub directory:

```
<hub_dir>/
  hub.json      ← contains broker_endpoint (read by hub_broker_endpoint())
  hub.pubkey    ← CurveZMQ public key     (read by hub_pubkey_path())
```

### 3.4 Vault File Convention

The default vault file path for a role is:

```
<role_dir>/vault/<role_uid>.vault
```

When `auth.keyfile` is empty in the config, `RoleDirectory::default_keyfile(uid)` returns
this path. Vault files are always placed inside `vault/` (0700) to prevent world-readable
access to encrypted secret keys.

### 3.5 Schemas Subdirectory (HEP-CORE-0034)

`<role_dir>/schemas/` is **optional** and **non-authoritative**. It is a local cache of
schema JSON files used to construct registration payloads — the source of truth for any
running schema record is the hub (`HubState.schemas`, per HEP-CORE-0034 §11).

| Role | Use of `<role_dir>/schemas/` |
|---|---|
| Producer | Reads its own schema JSON to build path-B `REG_REQ` (sends `schema_id` + BLDS + hash + packing). |
| Consumer | Optional pre-flight: read local file to compute expected hash before issuing `CONSUMER_REG_REQ`. |
| Processor | Same as consumer for input; same as producer for output. |

A role with no `schemas/` directory is fully valid:

- Producer using a compile-time C++ schema (`PYLABHUB_SCHEMA` macros) builds BLDS in code.
- Producer adopting a hub global needs only `(hub, schema_id)` in its config — no local file.
- Consumer/processor can resolve via `SCHEMA_REQ` after handshake.

If a local file disagrees with the hub's record, the **hub wins** (registration fails with
`fingerprint_mismatch` REG_NACK; operator updates the local file). `RoleDirectory` does not
parse or load these files itself; that is the responsibility of `SchemaLibrary`
(HEP-CORE-0034 §4) when the role's config or scripting layer requests a named schema.

`RoleDirectory::subdir("schemas")` returns the path; no dedicated accessor is required.

---

## 4. `RoleDirectory` API

**Header**: `src/include/utils/role_directory.hpp`
**Namespace**: `pylabhub`
**Link target**: `pylabhub::utils` (no new target required)

```cpp
namespace pylabhub {

class RoleDirectory
{
public:
    // ── Construction ──────────────────────────────────────────────────────────

    /// Open an existing role directory.
    /// Weakly canonicalizes the path; does not validate subdirectories.
    static RoleDirectory open(const std::filesystem::path &base);

    /// Open from an explicit config file path (--config <path> mode).
    /// base is derived as config_path.parent_path().
    static RoleDirectory from_config_file(const std::filesystem::path &config_path);

    /// Create the standard layout: logs/, run/, vault/ (0700), script/python/.
    /// Idempotent: creates directories if they don't exist.
    /// Throws std::runtime_error if directory creation fails.
    static RoleDirectory create(const std::filesystem::path &base);

    // ── Standard paths ────────────────────────────────────────────────────────

    const std::filesystem::path &base() const noexcept;  ///< Role directory root (absolute)
    std::filesystem::path logs()   const;  ///< <base>/logs/
    std::filesystem::path run()    const;  ///< <base>/run/
    std::filesystem::path vault()  const;  ///< <base>/vault/   (always 0700 on POSIX)

    /// <base>/<filename> — canonical config file location.
    std::filesystem::path config_file(std::string_view filename) const;

    /// Custom subdirectory: <base>/<name>/
    /// For extension by custom role types (e.g. subdir("data"), subdir("cache")).
    std::filesystem::path subdir(std::string_view name) const;

    // ── Script path resolution ────────────────────────────────────────────────

    /// Resolve the script entry point from config fields.
    ///
    /// script_path: value of config "script.path" field. If relative, resolved
    ///              relative to base(). If empty, defaults to base().
    /// type:        value of config "script.type" field ("python", "lua", etc.).
    ///
    /// Returns: <resolved_script_path>/script/<type>/__init__.<ext>
    /// where <ext> is derived from type ("py" for "python", "lua" for "lua", etc.).
    ///
    /// Example: script_path=".", type="python" → <base>/script/python/__init__.py
    std::filesystem::path script_entry(std::string_view script_path,
                                       std::string_view type) const;

    // ── Vault helpers ─────────────────────────────────────────────────────────

    /// Default vault file: <vault()>/<uid>.vault
    /// Used when auth.keyfile is empty in config — ensures vault always lives
    /// inside the protected vault/ subdirectory.
    std::filesystem::path default_keyfile(std::string_view uid) const;

    // ── Hub reference resolution ──────────────────────────────────────────────

    /// Resolve a hub_dir reference from config JSON to an absolute path.
    /// hub_dir_value: the string from config (relative or absolute). If relative,
    ///               resolved relative to base(). Returns std::nullopt if empty.
    std::optional<std::filesystem::path>
    resolve_hub_dir(std::string_view hub_dir_value) const;

    /// Given a resolved hub directory, return the path to hub.pubkey.
    static std::filesystem::path
    hub_pubkey_path(const std::filesystem::path &hub_dir);

    /// Given a resolved hub directory, read the broker endpoint from hub.json.
    /// Throws std::runtime_error if hub.json is absent or malformed.
    static std::string
    hub_broker_endpoint(const std::filesystem::path &hub_dir);

    /// Given a resolved hub directory, read the first line of hub.pubkey.
    /// Returns empty string if the file does not exist.
    static std::string
    hub_broker_pubkey(const std::filesystem::path &hub_dir);

    // ── Security diagnostics ─────────────────────────────────────────────────

    /// Emit a hardcoded security warning to stderr when `keyfile` resolves to
    /// a path inside `role_base`.  Relative `keyfile` paths are resolved relative
    /// to `role_base`.  No-op when `keyfile` is empty or outside the role dir.
    ///
    /// Call from Config::from_directory() immediately after loading auth.keyfile.
    static void warn_if_keyfile_in_role_dir(const std::filesystem::path &role_base,
                                             const std::string           &keyfile);

    // ── Validation ───────────────────────────────────────────────────────────

    /// Returns true if logs/, run/, vault/, script/ all exist under base().
    bool has_standard_layout() const;

private:
    std::filesystem::path base_;
};

} // namespace pylabhub
```

### 4.1 Error handling

`open()`, `create()`, and `from_config_file()` throw `std::runtime_error` (or
`std::invalid_argument`) on invalid input. Path accessor methods (`logs()`, `vault()`,
etc.) are pure computation and never throw. `resolve_hub_dir()` and `hub_broker_endpoint()`
throw `std::runtime_error` if the hub directory is present but malformed.

### 4.2 Platform notes

On POSIX, `create()` sets `vault/` to mode `0700` immediately after `create_directories`.
On Windows, the directory ACL is set to owner-only using `SetFileSecurity`. If the
permission call fails, `create()` logs a warning but does not throw — the vault file
itself is still protected by `RoleVault` encryption.

---

## 5. `role_cli.hpp` API

**Header**: `src/include/utils/role_cli.hpp`
**Namespace**: `pylabhub::role_cli`
**Implementation**: header-only (`inline` functions) — no additional link dependency

```cpp
namespace pylabhub::role_cli {

// ── Argument parsing ──────────────────────────────────────────────────────────

/// Parsed result of parse_role_args().
struct RoleArgs
{
    std::string config_path;    ///< --config <path>
    std::string role_dir;       ///< positional <dir> or --init target
    std::string init_name;      ///< --name <name>  (for --init)
    std::string log_file;       ///< --log-file <path>
    bool        validate_only{false}; ///< --validate
    bool        keygen_only{false};   ///< --keygen
    bool        init_only{false};     ///< --init
};

/// Parse standard pyLabHub role CLI arguments.
///
/// role_name: "producer", "consumer", "processor", or any custom role name.
/// Abbreviates to first 4 chars in usage strings (prod/cons/proc).
///
/// Handled flags: --init [dir], --config <path>, --name <n>, --validate,
///                --keygen, --log-file <path>, --run (no-op), --help/-h,
///                <positional role_dir>.
///
/// Prints usage and calls std::exit(0) on --help.
/// Prints error and calls std::exit(1) on unknown flags or missing required args.
RoleArgs parse_role_args(int argc, char *argv[], const char *role_name);

// ── Name resolution (for --init) ─────────────────────────────────────────────

/// Resolve role name for --init.
/// Priority: cli_name (from --name) → interactive TTY prompt → error.
/// Returns std::nullopt if non-interactive and cli_name is empty
/// (error already printed to stderr).
///
/// prompt: e.g. "Producer name (human-readable, e.g. 'TempSensor'): "
std::optional<std::string>
resolve_init_name(const std::string &cli_name, const char *prompt);

// ── Password helpers ──────────────────────────────────────────────────────────

/// Returns true when stdin is an interactive terminal.
bool is_stdin_tty();

/// Read a password from the terminal without echoing.
/// POSIX: getpass(). Windows: temporarily disables ENABLE_ECHO_INPUT.
/// Only call when is_stdin_tty() is true.
std::string read_password_interactive(const char *role_name, const char *prompt);

/// Resolve vault password for unlock (single prompt).
/// Priority: PYLABHUB_ACTOR_PASSWORD env var → interactive terminal prompt.
/// Returns std::nullopt when non-interactive and env var is unset
/// (error already printed to stderr).
std::optional<std::string>
get_role_password(const char *role_name, const char *prompt);

/// Resolve vault password for new vault creation (requires confirmation).
/// Priority: PYLABHUB_ACTOR_PASSWORD env var → interactive prompt + confirm.
/// Returns std::nullopt on mismatch or when non-interactive without env var
/// (error already printed to stderr).
std::optional<std::string>
get_new_role_password(const char *role_name,
                      const char *prompt,
                      const char *confirm_prompt);

} // namespace pylabhub::role_cli
```

---

## 6. Integration with Existing Code

### 6.1 `Config::from_directory()` — all three role configs

**Before** (per-role, ~15 lines duplicated in each):
```cpp
ProducerConfig ProducerConfig::from_directory(const std::string &dir)
{
    namespace fs = std::filesystem;
    const fs::path config_path = fs::path(dir) / "producer.json";
    if (!fs::exists(config_path))
        throw std::runtime_error("producer.json not found in: " + dir);
    return from_json_file(config_path.string());
}
```

**After**:
```cpp
ProducerConfig ProducerConfig::from_directory(const std::string &dir)
{
    const auto role_dir = pylabhub::RoleDirectory::open(dir);
    return from_json_file(role_dir.config_file("producer.json").string());
}
```

### 6.2 Hub reference resolution in config parsing

**Before** (each config parses hub_dir independently, ~20 lines):
```cpp
if (!j.value("hub_dir", "").empty()) {
    const fs::path hub = fs::path(dir).parent_path() / j["hub_dir"].get<std::string>();
    // ... read hub.json broker_endpoint, read hub.pubkey ...
}
```

**After**:
```cpp
if (auto hub = role_dir.resolve_hub_dir(j.value("hub_dir", "")))
{
    config.broker_endpoint  = pylabhub::RoleDirectory::hub_broker_endpoint(*hub);
    config.broker_pubkey    = pylabhub::RoleDirectory::hub_pubkey_path(*hub).string();
}
```

### 6.3 Script host path resolution

**Before** (each script host independently resolves script path, ~8 lines):
```cpp
fs::path script_dir = config_.script_path.empty()
    ? fs::path(config_.role_dir) / "script" / config_.script_type
    : fs::path(config_.script_path) / "script" / config_.script_type;
fs::path entry = script_dir / "__init__.py";
```

**After**:
```cpp
const fs::path entry = role_dir_.script_entry(config_.script_path, config_.script_type);
```

### 6.4 `do_init()` in each binary

**Before** (per-binary, ~35 lines):
```cpp
static int do_init(const std::string &dir_str, const std::string &cli_name)
{
    namespace fs = std::filesystem;
    const fs::path prod_dir = dir_str.empty() ? fs::current_path() : fs::path(dir_str);
    std::error_code ec;
    fs::create_directories(prod_dir / "logs", ec);
    fs::create_directories(prod_dir / "run",  ec);
    fs::create_directories(prod_dir / "vault", ec);
    fs::create_directories(prod_dir / "script" / "python", ec);
    if (ec) { std::cerr << "Error: ..."; return 1; }
    const fs::path json_path = prod_dir / "producer.json";
    if (fs::exists(json_path)) { std::cerr << "Error: already exists"; return 1; }
    std::string prod_name;
    if (!cli_name.empty())      { prod_name = cli_name; }
    else if (is_stdin_tty())    { std::cout << "Producer name: "; std::getline(...); }
    else                        { std::cerr << "Error: --name required"; return 1; }
    // ... JSON template, Python template ...
}
```

**After** (~8 lines for the common part):
```cpp
static int do_init(const std::string &dir_str, const std::string &cli_name)
{
    pylabhub::RoleDirectory role_dir;
    try { role_dir = pylabhub::RoleDirectory::create(
              dir_str.empty() ? fs::current_path() : dir_str, "producer.json"); }
    catch (const std::exception &e) { std::cerr << "Error: " << e.what() << "\n"; return 1; }

    auto prod_name = pylabhub::role_cli::resolve_init_name(cli_name,
                         "Producer name (human-readable, e.g. 'TempSensor'): ");
    if (!prod_name) return 1;
    // ... JSON template, Python template (role-specific, unchanged) ...
}
```

### 6.5 `main()` argument parsing

**Before** (per-binary struct + function, ~70 lines):
```cpp
struct ProdArgs { std::string config_path; std::string prod_dir; ... };
ProdArgs parse_args(int argc, char *argv[]) { ... }
// in main():
const ProdArgs args = parse_args(argc, argv);
if (!args.prod_dir.empty()) config = ProducerConfig::from_directory(args.prod_dir);
```

**After**:
```cpp
// in main():
const auto args = pylabhub::role_cli::parse_role_args(argc, argv, "producer");
if (!args.role_dir.empty()) config = ProducerConfig::from_directory(args.role_dir);
```

### 6.6 Keygen password block

**Before** (per-binary, ~25 lines):
```cpp
std::string password;
if (const char *env = std::getenv("PYLABHUB_ACTOR_PASSWORD")) { password = env; }
else if (!scripting::is_stdin_tty()) { std::cerr << "Error: ..."; return 1; }
else {
    password = scripting::read_password_interactive(...);
    const std::string confirm = scripting::read_password_interactive(...);
    if (password != confirm) { std::cerr << "Error: ..."; return 1; }
}
```

**After**:
```cpp
const auto password = pylabhub::role_cli::get_new_role_password(
    "producer",
    "Producer vault password (empty = no encryption): ",
    "Confirm password: ");
if (!password) return 1;
```

---

## 7. Custom Role Binary — Full Example

With both headers, a custom C++ role binary follows this canonical pattern:

```cpp
// my_sensor_main.cpp
#include "utils/role_directory.hpp"
#include "utils/role_cli.hpp"
#include "utils/actor_vault.hpp"
#include "utils/uid_utils.hpp"
#include "plh_datahub.hpp"

namespace rc  = pylabhub::role_cli;
namespace uid = pylabhub::uid;
using pylabhub::RoleDirectory;

static int do_init(const std::string &dir_str, const std::string &cli_name)
{
    RoleDirectory role_dir;
    try { role_dir = RoleDirectory::create(dir_str.empty() ? "." : dir_str, "sensor.json"); }
    catch (const std::exception &e) { std::cerr << "Error: " << e.what() << "\n"; return 1; }

    auto name = rc::resolve_init_name(cli_name, "Sensor name: ");
    if (!name) return 1;

    // Write sensor.json and script template using role_dir.config_file("sensor.json")
    // and role_dir.script_entry(".", "python") for default paths.
    // ...
    return 0;
}

int main(int argc, char *argv[])
{
    const auto args = rc::parse_role_args(argc, argv, "sensor");
    if (args.init_only) return do_init(args.role_dir, args.init_name);

    // Load config ...
    RoleDirectory role_dir = args.role_dir.empty()
        ? RoleDirectory::from_config_file(args.config_path)
        : RoleDirectory::open(args.role_dir);

    if (args.keygen_only) {
        const auto pw = rc::get_new_role_password("sensor",
            "Sensor vault password: ", "Confirm: ");
        if (!pw) return 1;
        const auto vault = pylabhub::utils::RoleVault::create(
            role_dir.default_keyfile(sensor_uid).string(), sensor_uid, *pw);
        std::cout << "Vault written to: " << vault.path() << "\n"
                  << "  public_key: " << vault.public_key() << "\n";
        return 0;
    }

    // Unlock vault, start lifecycle, run sensor loop ...
    const auto pw = rc::get_role_password("sensor", "Sensor vault password: ");
    if (!pw) return 1;
    // ...
}
```

This example: sets up the standard directory layout, generates a standard UID, handles
`--init`/`--keygen`/`--validate`/`--config` uniformly with the official binaries, and
places the vault file in `vault/<uid>.key` — the security-correct location — automatically.

---

## 8. Security Properties

| Property | Mechanism |
|----------|-----------|
| Vault directory always 0700 | `RoleDirectory::create()` sets permissions before returning |
| Vault files always inside `vault/` | `default_keyfile(uid)` returns `<vault()>/<uid>.key`; API makes the secure path the easy path |
| Password never echoed | `read_password_interactive()` uses `getpass()` / `SetConsoleMode` |
| Password never in command-line args | API has no password parameter — only env var or TTY |
| New vault requires confirmation | `get_new_role_password()` always prompts twice (unless env var) |
| Hub pubkey path has one resolver | `RoleDirectory::hub_pubkey_path()` — one audit point |
| Vault outside role dir warning | `warn_if_keyfile_in_role_dir()` emits a hardcoded `stderr` warning when vault path resolves inside role dir (enables offline brute-force if role scripts exfiltrate it) |
| Relative path traversal | `resolve_hub_dir()` resolves relative to `base()` — can be validated for escaping if needed (future) |

---

## 9. What Does NOT Change

- `ProducerConfig`, `ConsumerConfig`, `ProcessorConfig` remain the authority on
  role-specific config content (field names, defaults, validation). `RoleDirectory`
  is used only for path work inside `from_directory()` and script-path resolution.
- `HubConfig` is not affected — the hub directory has its own established loading
  pattern (`HubConfig::set_config_path()`). `RoleDirectory::hub_broker_endpoint()`
  and `hub_pubkey_path()` read the minimal hub info needed by role configs without
  going through `HubConfig`.
- Python scripting callbacks (`on_init`, `on_produce`, etc.) are unaffected.
- `role_main_helpers.hpp` remains as a private `scripting`-layer header for the
  lifecycle and monitoring boilerplate that requires `plh_datahub.hpp`. After this
  HEP, its password helpers and `is_stdin_tty()` move to the public `role_cli.hpp`;
  `role_main_helpers.hpp` retains only `role_lifecycle_modules()`,
  `register_signal_handler_lifecycle()`, and `run_role_main_loop()`.

---

## 10. Registration-Based Directory Initialization

> Added 2026-04-16. Extends §4 with `init_directory()` and role registration.

### 10.1 Problem

The `do_init()` function is duplicated in each binary's `main.cpp` (~120 lines each).
The common scaffolding (directory creation, name prompting, UID generation, file writing,
summary printing) is identical; only the config JSON template and optional post-init
actions differ per role. Adding a new role requires copying and editing an entire
`do_init()` function.

### 10.2 Design: Registration + Generic Init

`RoleDirectory` provides a generic `init_directory()` method. Role-specific content
is injected via a registration API — each role registers a `RoleInitInfo` struct once.
`RoleDirectory` stays generic and role-agnostic.

```cpp
/// Role-specific content for init_directory(). Registered once per role tag.
struct RoleInitInfo
{
    std::string config_filename;   ///< "producer.json", "consumer.json", etc.
    std::string uid_prefix;        ///< "PROD", "CONS", "PROC" — passed to generate_uid()
    std::string role_label;        ///< "Producer" — used in prompts and summary output

    /// Build the default JSON config template for this role.
    /// uid and name are already resolved by init_directory().
    std::function<nlohmann::json(const std::string &uid,
                                  const std::string &name)> config_template;

    /// Optional post-init callback for role-specific customization.
    /// Called after directory structure and config file are written.
    /// Use role_dir path APIs (script_entry, subdir, etc.) to locate paths.
    /// nullptr = no post-init action.
    std::function<void(const RoleDirectory &role_dir,
                        const std::string &name)> on_init;
};
```

### 10.3 Registration API

```cpp
class RoleDirectory
{
public:
    // ... existing API (§4) ...

    /// Register role-specific init content. Called once per role at startup.
    /// role_tag: "producer", "consumer", "processor", or any custom role tag.
    /// Overwrites any previous registration for the same tag.
    static void register_role(const std::string &role_tag, RoleInitInfo info);

    /// Scaffolding init for a registered role.
    ///
    /// 1. Calls create(dir) for directory structure
    /// 2. Resolves name (interactive prompt if empty, using role_label from registration)
    /// 3. Generates UID via uid_prefix from registration
    /// 4. Writes config_template() to config_filename in dir
    /// 5. Calls on_init(role_dir, name) if registered (role-specific customization)
    /// 6. Prints summary (directory, UID, name, config path, next steps)
    ///
    /// Returns 0 on success, non-zero on error (matches main() convention).
    static int init_directory(const std::filesystem::path &dir,
                              const std::string &role_tag,
                              const std::string &name = {});
};
```

### 10.4 Role Registration Example

Each role registers in its own source file. The registration happens at static
init time or early in `main()` — before `init_directory()` is called.

```cpp
// In producer_fields.cpp (compiled into pylabhub-utils)

void pylabhub::producer::register_producer_init()
{
    RoleDirectory::register_role("producer", {
        .config_filename = "producer.json",
        .uid_prefix      = "PROD",
        .role_label      = "Producer",
        .config_template = [](const std::string &uid, const std::string &name)
            -> nlohmann::json
        {
            nlohmann::json j;
            j["producer"]["uid"]       = uid;
            j["producer"]["name"]      = name;
            j["producer"]["log_level"] = "info";
            // ... role-specific default fields ...
            return j;
        },
        .on_init = [](const RoleDirectory &rd, const std::string &name)
        {
            // Write starter Python script at the standard script entry point
            const auto script_path = rd.script_entry(".", "python");
            std::ofstream out(script_path);
            out << "# Producer: " << name << "\n"
                << "def on_produce(tx, msgs, api):\n"
                << "    return True\n";
        },
    });
}
```

### 10.5 Binary Migration

The binary's `do_init()` reduces to a single dispatch:

```cpp
// Before: ~120 lines of duplicated scaffolding per binary
static int do_init(const std::string &dir, const std::string &name)
{
    // ... 120 lines of directory creation, JSON template, Python template ...
}

// After: one line
static int do_init(const std::string &dir, const std::string &name)
{
    return pylabhub::RoleDirectory::init_directory(dir, "producer", name);
}
```

### 10.6 `on_init` Callback and RoleDirectory Path APIs

The `on_init` callback receives a `const RoleDirectory &` which provides path
APIs for locating standard directories without hardcoding paths:

| Method | Returns | Example |
|--------|---------|---------|
| `base()` | Role directory root | `/home/user/my_producer/` |
| `logs()` | `<base>/logs/` | Log file directory |
| `run()` | `<base>/run/` | PID file, lock file directory |
| `vault()` | `<base>/vault/` | Vault files (0700) |
| `config_file(name)` | `<base>/<name>` | `<base>/producer.json` |
| `script_entry(path, type)` | Resolved script entry point | `<base>/script/python/__init__.py` |
| `subdir(name)` | `<base>/<name>/` | Custom subdirectory |

The callback uses these APIs to write role-specific files in the correct
locations. `RoleDirectory` does not know about scripts, templates, or any
role-specific content — it only provides the path framework.

### 10.7 Custom Role Registration

A custom role binary registers its own init content using
`RoleDirectory::register_role`, which returns a fluent
`RoleRegistrationBuilder`. The builder's setters take `std::function`
callbacks — std::function objects are constructed inside the shared
library so no function-object crosses the DSO boundary (ABI safe).

```cpp
int main(int argc, char *argv[])
{
    // Register before parsing args
    RoleDirectory::register_role("sensor")
        .config_filename("sensor.json")
        .uid_prefix("SENS")
        .role_label("Sensor")
        .config_template([](const std::string &uid, const std::string &name) {
            // Return the JSON template body for sensor.json (with uid/name
            // substituted). See producer_init.cpp for the canonical layout.
            return /* nlohmann::json template */;
        })
        .on_init([](const RoleDirectory &dir, const std::string &name) {
            // Optional post-init hook (create extra files, etc.).
        });
    // The builder commits on destruction; explicit .commit() also available.

    const auto parsed = role_cli::parse_role_args(argc, argv, "sensor");
    if (parsed.exit_code >= 0) return parsed.exit_code;
    const auto &args = parsed.args;
    if (args.init_only)
        return RoleDirectory::init_directory(args.role_dir, "sensor", args.init_name);
    // ...
}
```

No subclassing. No changes to `RoleDirectory`. The registration API is the
extension point.

> **ABI note**: `RoleRegistrationBuilder` (this section, init-time) takes
> **std::function** because the library owns the construction. By contrast,
> `RoleRegistry::register_runtime` / `RoleRuntimeInfo::host_factory`
> (§11 / §16.3 Step 4, runtime-time) takes a **plain function pointer** —
> there the function value crosses from the binary into the library's stored
> entry, so std::function would be ABI-fragile.

---

## 11. Unified Role Binary

> Added 2026-04-16. Replaces three separate binaries with one.

### 11.1 Motivation

The three role binaries (`pylabhub-producer`, `pylabhub-consumer`,
`pylabhub-processor`) share ~80% of their `main()` code. The differences are:

1. Role tag string and field parser function (config loading)
2. Role host class constructor
3. Binary name in signal handler / status display
4. `do_init()` content (migrating to `init_directory()` registration, §10)

With role hosts now compiled into `pylabhub-utils` (shared lib), the binaries
are thin wrappers. A single binary eliminates the duplication entirely.

### 11.2 CLI Design

```
pylabhub-role --role <tag> <dir>                    # Run role from directory
pylabhub-role --role <tag> --config <path>           # Run role from config file
pylabhub-role --role <tag> --init [<dir>]             # Init directory for role
pylabhub-role --role <tag> --config <path> --validate # Validate config
pylabhub-role --role <tag> --config <path> --keygen   # Generate keypair
```

The `--role` flag is required. Valid values: `producer`, `consumer`, `processor`,
or any registered custom role tag.

### 11.3 Role Tag Inference (optional convenience)

When `--role` is omitted, the binary attempts to infer the role:

1. **From binary name**: if invoked as `pylabhub-producer` (symlink or renamed),
   infer `--role producer`.
2. **From config file name**: if `--config producer.json`, infer `--role producer`.
3. **From directory contents**: if `<dir>/producer.json` exists, infer `--role producer`.

If inference fails, print error and exit.

### 11.4 Runtime Registration

Each role registers its runtime components alongside its init content:

```cpp
struct RoleRuntimeInfo
{
    /// Config field parser (role-specific JSON fields).
    std::function<void(const nlohmann::json &, pylabhub::config::RoleConfig &)> field_parser;

    /// Construct and return the role host. Takes ownership of config and engine.
    std::function<std::unique_ptr<RoleHostBase>(
        pylabhub::config::RoleConfig config,
        std::unique_ptr<pylabhub::scripting::ScriptEngine> engine,
        std::atomic<bool> *shutdown_flag)> create_host;

    /// Status line for signal handler display.
    std::function<std::string(const pylabhub::config::RoleConfig &,
                               std::chrono::seconds uptime)> status_line;
};

// Registration:
static void register_role_runtime(const std::string &role_tag, RoleRuntimeInfo info);
```

### 11.5 Unified `main()` Flow

```
main(argc, argv):
    1. Install signal handler
    2. Parse args (role_tag from --role or inference)
    3. if --init: return init_directory(dir, role_tag, name)
    4. Lifecycle init
    5. Load config via registered field_parser for role_tag
    6. if --keygen: keygen flow (generic, parameterized by role_tag)
    7. Auth (generic)
    8. Create engine (generic — same for all roles)
    9. Create host via registered create_host for role_tag
    10. startup, validate, run loop, shutdown (generic)
```

Steps 3, 5, 9 dispatch to registered role-specific functions.
Everything else is role-agnostic.

### 11.6 Legacy Binary Compatibility

The three original binary names are preserved as symlinks to the unified binary:

```
pylabhub-producer  →  pylabhub-role    (infers --role producer from binary name)
pylabhub-consumer  →  pylabhub-role    (infers --role consumer)
pylabhub-processor →  pylabhub-role    (infers --role processor)
```

Existing scripts and deployment configurations continue to work unchanged.
The symlinks can be created by CMake at install/staging time.

### 11.7 Architecture Diagram

```mermaid
flowchart TD
    subgraph Binary["pylabhub-role (unified binary)"]
        CLI["CLI: parse --role / infer from binary name"]
        REG["Role Registry\n(RoleInitInfo + RoleRuntimeInfo)"]
        INIT["init_directory()\ndispatch to registered role"]
        LOAD["Load config\nvia registered field_parser"]
        ENGINE["Create engine\n(Python/Lua/Native)"]
        HOST["Create host\nvia registered create_host"]
        RUN["startup → run_data_loop → shutdown"]
    end

    subgraph Lib["pylabhub-utils (shared lib)"]
        RD["RoleDirectory\ncreate / init_directory / path APIs"]
        RAB["RoleAPIBase\nqueue verbs, metrics, broker"]
        DL["run_data_loop&lt;Ops&gt;\ntemplate data loop"]
        CO["CycleOps\nProducer/Consumer/Processor"]
        RH["RoleHosts\nProducer/Consumer/ProcessorRoleHost"]
    end

    subgraph Roles["Role-specific (registered)"]
        P["Producer\nfields, config template, on_init, host"]
        C["Consumer\nfields, config template, on_init, host"]
        PR["Processor\nfields, config template, on_init, host"]
    end

    CLI --> REG
    REG --> INIT
    REG --> LOAD
    LOAD --> ENGINE --> HOST --> RUN
    INIT --> RD
    HOST --> RH
    RUN --> DL
    DL --> CO
    CO --> RAB
    P --> REG
    C --> REG
    PR --> REG
```

---

## 12. CLI ↔ Config Boundary

> Added 2026-04-17. Establishes the separation of concerns between the CLI and
> the config file, ensuring runtime behavior is driven entirely by the config.

### 12.1 Principle

The role config file is the **single source of truth for runtime behavior**.
CLI flags serve two purposes only:

1. **Init mode (`--init`)**: flags populate values into the new config file.
2. **Keygen mode (`--keygen`)**: flags identify the config and provide password
   input (no runtime behavior).

**Runtime mode (run)**: no config-altering flags are accepted. The only CLI
inputs are `--role <tag>`, the role directory (or `--config <path>`), and
operational flags that affect the execution environment (e.g., `--validate`).

This separation guarantees:
- **Reproducibility**: two operators starting the same role from the same
  config file get identical behavior.
- **Auditability**: what a role does is visible by reading one file.
- **No silent overrides**: there is no way to pass a CLI flag that changes
  what the role does at runtime, bypassing the config.

### 12.2 Flag classification

| Flag | Init | Keygen | Run |
|------|------|--------|-----|
| `--role <tag>` | ✅ required | ✅ required | ✅ required |
| positional `<dir>` | ✅ | — | ✅ (or `--config`) |
| `--config <path>` | — | ✅ | ✅ |
| `--name <name>` | ✅ | — | — |
| `--log-maxsize <N[KMG]>` | ✅ (writes `logging.max_size_mb`) | — | — |
| `--log-backups <N>` | ✅ (writes `logging.backups`) | — | — |
| `--log-file <path>` | ✅ (writes `logging.file_path`) | — | — |
| `--validate` | — | — | ✅ |
| `--keygen` | — | ✅ | — |
| `--init` | ✅ | — | — |

### 12.3 Mutual exclusion enforcement

`parse_role_args()` enforces the table:
- `--log-maxsize` / `--log-backups` / `--log-file` / `--name` outside of
  `--init` mode → error with usage message.
- `--validate` inside `--init` or `--keygen` mode → error.
- Unknown combinations → error.

### 12.4 LoggingConfig schema

New role config category (applies to all three roles):

```json
{
    "logging": {
        "file_path": "logs/<uid>.log",       // optional: overrides default path
        "max_size_mb": 10,                    // default 10 MiB
        "backups": 5,                         // default 5 backup files
        "timestamped": true                   // default true (new behavior)
    }
}
```

Defaults apply when fields are absent — new role configs generated by `--init`
omit `logging` entirely (all defaults). CLI flags at `--init` time populate
non-default values only.

**Default log file path** (when `logging.file_path` is absent):
- `<role_dir>/logs/<uid>-<YYYY-MM-DD-HH-MM-SS>.log` (timestamped mode)
- `<role_dir>/logs/<uid>.log` (non-timestamped mode, explicit `"timestamped": false`)

### 12.5 RotatingFileSink extension

`RotatingFileSink` is extended with a `Mode` enum (not a new class):

```cpp
class RotatingFileSink : public Sink, private BaseFileSink
{
public:
    enum class Mode { Numeric, Timestamped };

    // Existing ctor — Numeric mode (backwards compatible):
    RotatingFileSink(const fs::path &base_path, size_t max_size,
                     size_t max_backups, bool use_flock);

    // New ctor with explicit mode:
    RotatingFileSink(const fs::path &base_path, size_t max_size,
                     size_t max_backups, bool use_flock, Mode mode);

private:
    Mode mode_{Mode::Numeric};
    // rotate() checks mode_ and dispatches internally.
};
```

Numeric mode (existing): rotates `base → base.1`, `base.1 → base.2`, ...
Timestamped mode (new): opens `base-<creation-time>.log`, on rotation closes
current file and opens new one with fresh timestamp; cleans oldest files in
the directory matching `<basename>-*.log` to enforce `max_backups`.

`RotatingLogConfig` gains `timestamped_names` boolean flag (default false for
backwards compat; `plh_role` sets it true).

---

## 13. Updated Implementation Plan

### Original phases (2026-03-12)

| Phase | Scope | Status |
|-------|-------|--------|
| 1 | `RoleDirectory` class — layout, path resolution, hub resolution | ✅ 2026-03-12 |
| 2 | `role_cli.hpp` — `RoleArgs`, `parse_role_args`, password helpers | ✅ 2026-03-12 |
| 3 | Migrate `Config::from_directory()` in all 3 role configs | ✅ 2026-03-12 |
| 4 | Migrate script-path resolution in all 3 script hosts | ⚪ deferred |
| 5 | Migrate `do_init()` and `main()` to use `RoleDirectory` + `role_cli` | ✅ 2026-03-12 |
| 6 | Move password helpers to `role_cli.hpp` | ✅ 2026-03-12 |
| 7 | Update README docs with new public API | ⚪ deferred |
| 8 | Tests: `RoleDirectoryTest` + `RoleCliTest` | ✅ 2026-03-12 |

### New phases (2026-04-16..17)

| # | Scope | Status |
|---|-------|--------|
| 9  | `RoleInitInfo` struct + `register_role()` + `init_directory()` on `RoleDirectory` | ✅ 2026-04-16 |
| 10 | Register producer/consumer/processor init content; migrate `do_init()` to one-line dispatch | ✅ 2026-04-16 |
| 11 | Role hosts moved into `pylabhub-utils` shared lib | ✅ 2026-04-16 |
| 12 | Rename `pylabhub-pyenv` → `plh_pyenv` | ✅ 2026-04-17 |
| 13 | `RotatingFileSink::Mode::Timestamped` extension; `RotatingLogConfig::timestamped_names` | ✅ 2026-04-17 |
| 14 | `LoggingConfig` category in `RoleConfig` with parser + whitelist | ✅ 2026-04-17 |
| 15 | `RoleHostBase` abstract class (lib-internal) + three role hosts inherit | ✅ 2026-04-18 |
| 16 | `RoleRuntimeInfo` struct + `register_runtime()` builder + `get_runtime()` accessor | ✅ 2026-04-18 |
| 17 | Register producer/consumer/processor runtime info + `bootstrap_<role>()` per role | ✅ 2026-04-19 |
| 18 | Extend `role_cli`: `--role`, `--log-maxsize`, `--log-backups`, `--log-file`; flag mode-exclusion enforcement | ✅ 2026-04-19 |
| 19 | `plh_role` unified binary + CMake target | ✅ 2026-04-20 |
| 20 | Delete `pylabhub-producer/consumer/processor` binaries | ✅ 2026-04-21 |
| 21 | Migrate/unify L4 tests to parameterize on role tag via `plh_role` (71 tests in `test_layer4_plh_role/`) | ✅ 2026-04-21 |
| 22 | Update README / Deployment docs for `plh_role` + new CLI surface | ✅ 2026-04-21 |

**Closure note (2026-04-21)**: All HEP-0024 phases complete. System-level L4
tests (broker round-trip, pipeline, channel broadcast, processor pipeline,
hub-dead, inbox) are **out of scope** for this HEP — they are
system-integration tests and land with HEP-CORE-0033 Hub Character (where a
working hub binary exists). Tracked in `docs/todo/MESSAGEHUB_TODO.md`.

### Commit order

1. **Logger changes** (phase 13 → low risk, backwards-compatible extension)
2. **Config schema** (phase 14 → `LoggingConfig` category)
3. **Lib infrastructure** (phases 15, 16)
4. **Per-role runtime registration** (phase 17)
5. **CLI extension** (phase 18)
6. **Binary creation** (phase 19)
7. **Test migration** (phase 21) + **binary deletion** (phase 20) — same commit
8. **Docs** (phase 22)

---

## 14. Source File Reference

Updated 2026-04-21 to reflect the post-unification layout (per-role
`*_main.cpp` deleted; per-role config parsing merged into `RoleConfig`).

| Component | File |
|-----------|------|
| `RoleDirectory` class | `src/include/utils/role_directory.hpp` (public), `src/utils/config/role_directory.cpp` |
| `role_cli.hpp` | `src/include/utils/role_cli.hpp` (public, header-only) |
| `RoleConfig` composite | `src/include/utils/config/role_config.hpp`, `src/utils/config/role_config.cpp` |
| Role-specific field parsers | `src/{producer,consumer,processor}/{producer,consumer,processor}_fields.hpp` |
| `RoleHostBase` abstract | `src/include/utils/role_host_base.hpp` |
| `RoleHostCore` shared state | `src/include/utils/role_host_core.hpp` (Pimpl'd) |
| Per-role hosts (derive from `RoleHostBase`) | `src/{producer,consumer,processor}/*_role_host.cpp/.hpp` |
| Per-role init registration (`register_<role>_init`) | `src/{producer,consumer,processor}/*_init.cpp/.hpp` |
| Per-role runtime registration (`register_<role>_runtime`) | same file as `_init.cpp` above |
| `CycleOps` classes (one per role shape) | `src/utils/service/cycle_ops.hpp` |
| Per-role pybind11 / Lua API | `src/{producer,consumer,processor}/*_api.cpp/.hpp` |
| `plh_role` unified binary main | `src/plh_role/plh_role_main.cpp` |
| L4 no-hub-tier tests | `tests/test_layer4_plh_role/` (71 tests) |
| Embedded API guide | `docs/README/README_EmbeddedAPI.md` |

---

## 15. Role Plurality — Design Rationale

> Added 2026-04-21 after the unification was ratified as the final design.
> See also: archived `docs/archive/transient-2026-04-21/role_unification_design.md`
> for the historical refactor plan and the divergence rationale in-line.

### 15.1 What is role-neutral vs role-specific

After HEP-CORE-0024 Phases 15-22, the framework's role handling is split
into two layers:

| Layer | Role-neutral | Role-specific |
|-------|--------------|---------------|
| Binary | `plh_role` single executable | — |
| CLI parsing | `role_cli::parse_role_args` | `--role <tag>` selector |
| Config factory | `RoleConfig::load(path, role_tag, parser)` | role-specific field parser injected |
| Host base | `RoleHostBase` abstract | three concrete hosts derive |
| Host core state | `RoleHostCore` (pimpl'd) | — |
| Data loop frame | `run_data_loop<CycleOps>` template | three `CycleOps` concrete classes |
| Queue abstractions | `QueueWriter` / `QueueReader` | — |
| Script engine | `ScriptEngine` interface + `invoke(name)` / `eval` generic paths | `invoke_produce` / `invoke_consume` / `invoke_process` typed paths |
| Lifecycle helper | `scripting::role_lifecycle_modules()` | — |
| Protocol / broker | BrokerService (broker side), BrokerRequestComm (role side) | — |
| Band membership / pub-sub | `band_join` / `band_leave` / `band_broadcast` / `band_members` on `RoleAPIBase`; `BandRegistry` broker-side; `BAND_*_{REQ,ACK,NOTIFY}` protocol (HEP-CORE-0030) | — |

**Role identity is deliberately plural at three levels**: (a) `CycleOps`
class, (b) `RoleHostBase` subclass, (c) role config section. Everything
between is role-neutral.

### 15.2 Why three `CycleOps` classes (not one)

`src/utils/service/cycle_ops.hpp` defines `ProducerCycleOps`,
`ConsumerCycleOps`, `ProcessorCycleOps` as `final` concrete classes, each
~100 lines. The data-loop template `run_data_loop<Ops>` duck-types on
`Ops::acquire()`, `Ops::invoke_and_commit()`, `Ops::cleanup_on_*()`.

The consolidation `3 classes → 1` was explicitly evaluated (as L3.β in the
role-unification draft) and **declined**:

1. **Processor has a fundamentally different cycle shape.** Its `acquire()`
   takes two slots (input + output), holds the input across cycles when
   backpressure denies the output slot, and has two timeout policies
   (drop-mode vs block-mode). Producer and consumer each take one slot
   with no cross-cycle hold. Unifying requires either boolean flags
   (`has_input_side`, `has_output_side`, `hold_on_backpressure`, …) or
   variant state — both of which obscure per-role intent more than three
   small focused classes do.
2. **Template specialization beats runtime branching.** `run_data_loop`
   instantiates specialized code per role; the compiler inlines each
   operation. Replacing this with a generic `CycleOps` that branches on
   role kind at runtime would give up this optimization without any
   architectural benefit.
3. **Each class's state is exactly what that role needs.** Producer
   carries `buf_`; consumer carries `data_`; processor carries
   `held_input_ + out_buf_ + drop_mode_ + two item sizes`. Three focused
   structs are more readable than one union-of-all.
4. **Role identity is already contained.** The fact that there are three
   classes doesn't pollute the rest of the framework — `run_data_loop`,
   `RoleAPIBase`, `RoleHostCore`, broker protocol, and SHM/ZMQ queue
   layers have no idea which role is running. The plurality is bounded
   to exactly where the cycle shape genuinely differs.

### 15.3 Why typed `invoke_*` methods (alongside generic `invoke`)

`ScriptEngine` exposes both paths in parallel:

- **Generic path**: `invoke(const std::string &name)`,
  `invoke(const std::string &name, const nlohmann::json &args)`, and
  `eval(const std::string &code)` — all virtual, used for lifecycle
  callbacks (`on_start`, `on_stop`, `on_heartbeat`) and arbitrary
  user-defined callbacks including future hub events.
- **Typed path** (zero-copy, slot-bound):
  - `invoke_produce(InvokeTx tx, std::vector<IncomingMessage> &msgs)` →
    drives `on_produce`.
  - `invoke_consume(InvokeRx rx, std::vector<IncomingMessage> &msgs)` →
    drives `on_consume`.
  - `invoke_process(InvokeRx rx, InvokeTx tx, std::vector<IncomingMessage> &msgs)`
    → drives `on_process`.
  - `invoke_on_inbox(InvokeInbox msg)` → drives `on_inbox` for inbox
    messaging (HEP-CORE-0027).

The typed path exists because **`InvokeTx` / `InvokeRx` / `InvokeInbox`
carry raw pointers** into shared-memory slots / inbox frames acquired via
`write_acquire` / `read_acquire` / `recv_one`. These pointers cannot
round-trip through JSON — serialising them defeats the zero-copy design.
The typed methods preserve that contract; collapsing them into
`invoke(name, json_args)` would either break zero-copy or require a
parallel typed path anyway. (Adding a future typed invoke for a new
zero-copy pattern follows the same pattern: a new struct + a new typed
virtual method.)

### 15.4 What this means for extension

- **Adding a custom script callback** (no new data-plane semantics) →
  use the generic `invoke(name)` / `invoke(name, args_json)`. No engine
  change; script just defines a function of the matching name.
- **Adding a new role whose cycle shape matches an existing role** →
  new config, new `RoleHostBase` subclass, but reuse an existing
  `CycleOps` class and an existing `invoke_*` method.
- **Adding a new role with a genuinely different cycle shape** → new
  `CycleOps` class; may or may not need a new `invoke_*` method
  depending on whether slot zero-copy is required.

See §16 for the full add-a-new-role checklist.

---

## 16. Adding a New Role — End-to-End Checklist

This section walks through everything needed to add a hypothetical new
role `sensor` to the framework. The checklist assumes the new role fits
the role-directory + plh_role binary model — i.e., it's a data-plane role
that launches as `plh_role --role sensor <sensor_dir>` and uses the
standard directory layout (`sensor.json`, `vault/`, `logs/`, `run/`,
`script/<type>/`).

### 16.1 Decide cycle shape first

This decision determines how much code you write.

| Cycle shape | What to do | Work scope |
|-------------|------------|------------|
| **Matches producer** (single out-side, no input) | Reuse `ProducerCycleOps` | Minimal — ~4 new files |
| **Matches consumer** (single in-side, no output) | Reuse `ConsumerCycleOps` | Minimal — ~4 new files |
| **Matches processor** (in + out, optional hold) | Reuse `ProcessorCycleOps` | Minimal — ~4 new files |
| **Genuinely different** (e.g. bidirectional, multi-slot batch, event-triggered) | Write a new `CycleOps` class | Add ~100 LOC to `cycle_ops.hpp` + small `ScriptEngine` typed method if zero-copy needed |

### 16.2 File inventory for a new role

For each new role, create:

```
src/<role>/
  <role>_fields.hpp         — role-specific config fields struct
  <role>_init.hpp / .cpp    — register_<role>_init()    (directory template)
                              register_<role>_runtime() (plh_role dispatch entry)
  <role>_role_host.hpp/.cpp — class <Role>RoleHost : public RoleHostBase
  <role>_api.hpp / .cpp     — <Role>API class + pybind11 / Lua bindings
```

(Compiled into `pylabhub-utils` + `pylabhub-scripting` via the absolute
paths in `src/utils/CMakeLists.txt` + `src/scripting/CMakeLists.txt` —
same as the existing three roles.)

### 16.3 Implementation steps (in order)

**Step 1 — Config fields.**
`<role>_fields.hpp` defines the role-specific config struct (e.g.
`struct SensorFields { sampling_rate_hz, channel_count, ... }`) and a
free function with the exact signature

```cpp
inline std::any parse_sensor_fields(const nlohmann::json &j,
                                     const pylabhub::config::RoleConfig &cfg);
```

that extracts those fields and returns them wrapped in `std::any`. This
function becomes the `config_parser` slot in `RoleRuntimeInfo` (Step 4)
and is what `RoleConfig::load_from_directory` uses to populate
`role_data<SensorFields>()`. See `src/producer/producer_fields.hpp` for
the canonical pattern.

**Step 2 — Init registration.**
`<role>_init.cpp` defines a free function `register_sensor_init()` that
calls the `RoleDirectory::register_role` fluent builder. The builder's
setters take `std::function` callbacks (constructed inside the shared
library — ABI safe, see §10.7):

```cpp
void register_sensor_init()
{
    RoleDirectory::register_role("sensor")
        .config_filename("sensor.json")
        .uid_prefix("SENS")
        .role_label("Sensor")
        .config_template([](const std::string &uid, const std::string &name) {
            // Return the JSON template body for sensor.json.
            // See producer_init.cpp for the canonical layout.
            return /* nlohmann::json template */;
        })
        .on_init([](const RoleDirectory &dir, const std::string &name) {
            // Optional post-init hook (create extra files, etc.).
        });
    // The builder commits on destruction; explicit .commit() also available.
}
```

Note: `RoleRegistrationBuilder` (init time) takes `std::function`
because the library owns construction. By contrast, `register_runtime`
(Step 4, runtime time) takes plain function pointers — different ABI
constraints. See §10.7 ABI note.

**Step 3 — Role host subclass + `worker_main_()` override (the substantial work).**

Define `class SensorRoleHost : public scripting::RoleHostBase` with the
standard constructor signature `(config::RoleConfig, std::unique_ptr<scripting::ScriptEngine>, std::atomic<bool>*)`.
Then **override `worker_main_()`** — this is **pure virtual**
(`role_host_base.hpp:174`); there is no default. Inside the override the
host runs the role's full lifecycle on the worker thread.

Producer's `worker_main_()` (`src/producer/producer_role_host.cpp:75-435`)
is the canonical reference, ~360 LOC organised into **14 numbered
sub-steps**:

| Sub-step | Purpose |
|---|---|
| 1  | Resolve schemas from config (`role_data<RoleFields>()`) |
| 2  | Setup infrastructure (queues, no engine yet) |
| 3  | Wire infrastructure onto the role's `RoleAPIBase` |
| 4  | Load engine via lifecycle module (`engine_lifecycle_startup`) |
| 5  | Invoke `on_init` script callback |
| 6  | Connect to broker, start ctrl thread, register |
| 6b | Startup coordination — wait for prerequisite roles (HEP-CORE-0023) |
| 7  | Signal startup-promise ready (so `RoleHostBase::startup_()` returns) |
| **8** | **Construct CycleOps + call `run_data_loop` — blocks until shutdown** |
| 9  | Stop accepting invoke from non-owner threads |
| 9a | Deregister from broker (ctrl thread still alive) |
| 10 | Last script callback (e.g. `on_stop`) — ctrl thread still up so script can ack |
| 11 | Finalize engine (free script resources) |
| 12 | Signal ctrl thread's poll loop to exit |
| 13 | Teardown infrastructure (disconnect broker, close inbox/queues) |
| 14 | Drain all managed threads (last) |

A new role's `worker_main_()` follows the same skeleton. **Sub-step 8 is
where CycleOps is selected and constructed** — see Step 6 below for the
selection decision and the exact `run_data_loop` call site.

**Step 4 — Runtime registration.**
`<role>_init.cpp` also defines a free **factory function** (NOT a lambda
— `RoleRuntimeInfo::host_factory` is an ABI-stable function pointer that
capturing lambdas don't convert to) plus `register_sensor_runtime()`
which calls the fluent `RoleRegistry::register_runtime` builder:

```cpp
namespace {

// Free function — convertible to RoleRuntimeInfo::HostFactory function
// pointer (lambdas with captures are NOT — see role_registry.hpp).
std::unique_ptr<scripting::RoleHostBase> make_sensor_host(
    config::RoleConfig config,
    std::unique_ptr<scripting::ScriptEngine> engine,
    std::atomic<bool> *shutdown_flag)
{
    return std::make_unique<SensorRoleHost>(
        std::move(config), std::move(engine), shutdown_flag);
}

} // namespace

void register_sensor_runtime()
{
    utils::RoleRegistry::register_runtime("sensor")
        .role_label("Sensor")
        .host_factory(&make_sensor_host)
        .config_parser(&parse_sensor_fields)
        .commit();   // .commit() is required to insert the entry; the
                     // RuntimeBuilder destructor commits as a fallback.
}
```

This is the exact pattern used in `src/producer/producer_init.cpp:147-165`
for `make_producer_host` + `register_producer_runtime`. Reuse that as
the canonical reference.

**Step 5 — `plh_role` dispatch map.**
Extend `kRegistrars()` in `src/plh_role/plh_role_main.cpp` (or in a
downstream fork — see §10.7 for the "no-subclassing, register-only" model
for out-of-tree custom roles):
```cpp
{"sensor", {&register_sensor_init, &register_sensor_runtime}},
```

**Step 6 — CycleOps decision (executed inside Step 3 sub-step 8).**

Decide which `CycleOps` your role uses. There is no virtual-base or
template-hook indirection (intentional — see §15.2); the choice is
expressed directly by the type you instantiate at the `run_data_loop`
call site inside `worker_main_()`:

```cpp
// Sub-step 8 in worker_main_(): construct CycleOps + run loop.
ProducerCycleOps ops(api_ref, engine_ref, core_, sc.stop_on_script_error);
//   ^^^ pick ProducerCycleOps / ConsumerCycleOps / ProcessorCycleOps,
//       OR a new SensorCycleOps you wrote (see below).
scripting::LoopConfig lcfg{ /* period_us, loop_timing, ... from config_ */ };
scripting::run_data_loop(api_ref, core_, lcfg, ops);
//             ^^^ template arg deduced from `ops` — no explicit <Ops>.
```

**Decision logic:**

| New role's cycle shape | Action |
|---|---|
| Single-side output (matches producer) | Use existing `ProducerCycleOps`. No new file. |
| Single-side input (matches consumer) | Use existing `ConsumerCycleOps`. No new file. |
| Dual-side with input-hold + drop/block timeout (matches processor) | Use existing `ProcessorCycleOps`. No new file. |
| Genuinely different (e.g. multi-slot batch, event-triggered, bidirectional with different policy) | Add a new `SensorCycleOps` class (`final`, ~100 LOC) to `src/utils/service/cycle_ops.hpp` with `acquire(ctx)` / `invoke_and_commit(msgs)` / `cleanup_on_shutdown()` / `cleanup_on_exit()` methods, then instantiate it in `worker_main_()` as above. |

**Most new roles will not need a new `CycleOps`** — the existing three
shapes cover the common patterns. If you do add one and it requires a
new typed `invoke_*` for zero-copy slot args (because `InvokeTx` /
`InvokeRx` / `InvokeInbox` don't fit), also add a parallel virtual
method to `ScriptEngine` with default + Python / Lua / Native engine
implementations. See §15.3 for why typed invokes coexist with the
generic `invoke(name, args_json)`.

**Step 7 — Script-facing API.**
`<role>_api.cpp` defines a `SensorAPI` class that **wraps** (composition,
not inheritance) a `scripting::RoleAPIBase &` — exactly the pattern used
by `ProducerAPI` / `ConsumerAPI` / `ProcessorAPI`. Sketch:

```cpp
class SensorAPI {
public:
    explicit SensorAPI(scripting::RoleAPIBase &base) : base_(&base) {}

    // Forward common queries to base_ (uid, name, channel, log_level, ...).
    [[nodiscard]] const std::string &uid() const noexcept { return base_->uid(); }
    // ... rest as in producer_api.hpp ...

    // Add role-specific methods on top, e.g.:
    void set_sampling_rate(double hz);

private:
    scripting::RoleAPIBase *base_;
};
```

Then expose pybind11 bindings on `SensorAPI` (and Lua bindings via the
LuaEngine pattern). See `src/producer/producer_api.hpp` for the
canonical layout. **Do not** inherit from `RoleAPIBase` — the existing
role APIs all use composition because RoleAPIBase carries lifecycle
state owned by the role host.

**Step 8 — Script callback name.**
Pick the primary callback name the role dispatches to (producer uses
`on_produce`, consumer `on_consume`, processor `on_process`). Use this
name in the role host's invocation and document it in `<role>_api.hpp`.

**Step 9 — CMake.**
Add the role's `.cpp` files to `src/utils/CMakeLists.txt`
(role_host + init) and `src/scripting/CMakeLists.txt` (api). No per-role
`CMakeLists.txt` is needed — the role files compile into `pylabhub-utils`
and `pylabhub-scripting` via absolute paths.

**Step 10 — L4 tests.**
Extend `tests/test_layer4_plh_role/` — most tests are parametrized over
role tag (`RoleSpec`), so adding a new role often just requires adding
it to the `INSTANTIATE_TEST_SUITE_P` list with role-specific
`uid_prefix`, `role_json_key`, and `default_loop_timing`. A few tests
(init-specific, role-specific field checks) may need dedicated cases.

### 16.4 What you do NOT need to touch

The following are role-neutral and stay unchanged for any new role:
- `RoleDirectory`, `RoleRegistry`, `RoleHostCore`, `RoleAPIBase`, `role_cli`.
- `run_data_loop`, `LoopTimingPolicy`, backoff strategies, heartbeat.
- `BrokerService` (broker side), `BrokerRequestComm` (role side), ZMQ context.
- **Band membership and pub-sub** (HEP-CORE-0030). `BandRegistry`,
  `BAND_*_{REQ,ACK,NOTIFY}` protocol on the broker side, and the
  `band_join` / `band_leave` / `band_broadcast` / `band_members` methods
  on `RoleAPIBase` are role-neutral. A new role inherits band access
  through `RoleAPIBase` automatically — no per-role wiring required.
- `ScriptEngine` interface (unless adding a new typed invoke method).
- `QueueReader` / `QueueWriter` abstractions and their SHM/ZMQ concretes.
- `RoleConfig` composite (role-specific fields go through the injected
  `RoleParser`; no class surgery required).
- `plh_role` binary structure (only the dispatch map extends).
- The L2 `role_cli` / `role_registry` / `engine_factory` tests.

### 16.5 Out-of-tree custom roles

A downstream project can register a custom role **without forking the
codebase**: link against `pylabhub-utils` + `pylabhub-scripting`, write
the 4 source files in §16.2 as a separate library, and write a thin main
that extends `kRegistrars()` before parsing args. See §10.7 for the
init-only API shape; runtime registration follows the same pattern.
