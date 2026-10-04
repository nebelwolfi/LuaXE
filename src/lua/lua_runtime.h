//
// ONE Lua runtime (TB-195).
//
// lxe.exe used to link LuaJIT STATICALLY (C:\LuaJIT-2.1.M.64\src\lua51.lib)
// while every LuaXE module (thread.dll, network.dll, lfs.dll, crypto.dll, ...)
// links shared\lib\lua51dyn.lib, an import library for lua51.dll. The process
// therefore held TWO LuaJITs: the exe's copy owned every lua_State and its heap
// while the modules' lua_* calls ran in lua51.dll's copy against those same
// states, and a cdata stores only an index into a per-state C type table that
// each luaopen_ffi replaces. Heap corruption (lj_alloc_free, lj_alloc_f <- gc_sweep)
// followed.
//
// This is the fix: lxe links no LuaJIT at all. The runtime is loaded at RUN time
// with LoadLibrary + GetProcAddress into function pointers, and the lua_* /
// luaL_* / luaopen_* symbols lxe itself calls are thin forwarding stubs defined in
// lua_runtime.cpp. Because the loaded image is named lua51.dll, the modules'
// load-time imports bind to that SAME image: one runtime in one address space.
//
// Nothing here loads an unverified binary: a lua51.dll found on disk or
// downloaded is checked against a pinned SHA-256 (and, always, against the export
// list lxe needs) before it is loaded.
//
#ifndef LUAXE_LUA_RUNTIME_H
#define LUAXE_LUA_RUNTIME_H

#include <filesystem>
#include <string>

namespace lua_runtime {

/// Name of the one runtime image. Every module resolves `lua51.dll` too, so the
/// base name is load-bearing, not cosmetic.
inline constexpr wchar_t kDllName[] = L"lua51.dll";

/// Load and bind the one LuaJIT this process uses. `bundled_dir` is where a
/// compiled executable's payload was extracted (may be empty); it is searched
/// before anything is downloaded. Returns false and fills `error` (when given)
/// with a message naming the file and what was wrong with it.
bool ensure(const std::filesystem::path& bundled_dir, std::string* error = nullptr);

/// True once the runtime is loaded and bound. A module can ask this instead of
/// downloading a second copy.
bool loaded();

/// Full path of the loaded lua51.dll, or "" when nothing is loaded.
const std::string& path();

/// One line for diagnostics: the runtime path, its base address and the address
/// of one of its entry points, so a caller can prove the exe's own lua_* calls
/// land in the same image the modules bind to.
std::string diagnostics();

/// Verify a file against the pinned hashes / the export list without loading it.
/// `reason` (when given) receives the verdict: "sha256 ... (known)", or
/// "sha256 ... is not a known-good LuaJIT build", or "missing export ...".
bool verify(const std::filesystem::path& file, std::string* reason);

/// True only when the file's SHA-256 is one of the pinned known-good builds -
/// unlike verify(), LUAXE_ALLOW_UNVERIFIED_LUA51 does not change this answer.
/// What a compiled program EMBEDS has to pass this: it will run elsewhere.
bool known_good(const std::filesystem::path& file);
/// The same check for bytes in memory (a runtime a payload carries), so only a
/// known-good image is ever written where every lxe looks for its runtime.
bool known_good_bytes(const std::string& data);

/// SHA-256 of a file, lowercase hex, or "" when it cannot be read.
std::string sha256(const std::filesystem::path& file);

} // namespace lua_runtime

#endif // LUAXE_LUA_RUNTIME_H