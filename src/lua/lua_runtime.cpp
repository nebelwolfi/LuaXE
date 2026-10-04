//
// ONE Lua runtime (TB-195) - see src/lua/lua_runtime.h for the why.
//
// lxe.exe links NO LuaJIT. Every lua_*/luaL_*/luaopen_*/ll_loadfunc call in
// lxe's own C++ goes through a forwarding stub defined here, which calls a
// function pointer resolved once with GetProcAddress on the loaded lua51.dll.
// Dropping a symbol lxe still needs therefore fails at LINK time, which is the
// completeness check for the table below.
//
#include "src/pch.h"
#include "src/lua/lua_runtime.h"
#include "src/https/connection/API.h"
#include <bcrypt.h>

// ---------------------------------------------------------------------------
// The runtime's table: return type, name, parameter types, argument names.
// Signatures are copied from shared/include/luaxe/{lua,lauxlib,lualib,luajit}.h
// plus LuaXE's two patched exports (ll_loadfunc, lib_package.c; lua_dump_strip,
// lj_load.c). A stock LuaJIT lacks ll_loadfunc and is refused.
//
// The stubs below live at GLOBAL scope, not in a namespace: an `extern "C"`
// definition inside an unnamed namespace does not reliably carry external
// linkage, and every other translation unit calls these names through the
// declarations in shared/include/luaxe.
// ---------------------------------------------------------------------------
#define LUA_RUNTIME_FUNCTIONS(X)                                                                                        \
    /* lua.h */                                                                                                        \
    X(lua_newstate, lua_State*, (lua_Alloc f, void* ud), (f, ud))                                                        \
    X(lua_close, void, (lua_State* L), (L))                                                                             \
    X(lua_newthread, lua_State*, (lua_State* L), (L))                                                                   \
    X(lua_atpanic, lua_CFunction, (lua_State* L, lua_CFunction panicf), (L, panicf))                                     \
    X(lua_gettop, int, (lua_State* L), (L))                                                                             \
    X(lua_settop, void, (lua_State* L, int idx), (L, idx))                                                              \
    X(lua_pushvalue, void, (lua_State* L, int idx), (L, idx))                                                           \
    X(lua_remove, void, (lua_State* L, int idx), (L, idx))                                                              \
    X(lua_insert, void, (lua_State* L, int idx), (L, idx))                                                              \
    X(lua_replace, void, (lua_State* L, int idx), (L, idx))                                                             \
    X(lua_checkstack, int, (lua_State* L, int sz), (L, sz))                                                             \
    X(lua_xmove, void, (lua_State* from, lua_State* to, int n), (from, to, n))                                          \
    X(lua_isnumber, int, (lua_State* L, int idx), (L, idx))                                                             \
    X(lua_isstring, int, (lua_State* L, int idx), (L, idx))                                                             \
    X(lua_iscfunction, int, (lua_State* L, int idx), (L, idx))                                                          \
    X(lua_isuserdata, int, (lua_State* L, int idx), (L, idx))                                                           \
    X(lua_type, int, (lua_State* L, int idx), (L, idx))                                                                 \
    X(lua_typename, const char*, (lua_State* L, int tp), (L, tp))                                                       \
    X(lua_equal, int, (lua_State* L, int idx1, int idx2), (L, idx1, idx2))                                              \
    X(lua_rawequal, int, (lua_State* L, int idx1, int idx2), (L, idx1, idx2))                                           \
    X(lua_lessthan, int, (lua_State* L, int idx1, int idx2), (L, idx1, idx2))                                           \
    X(lua_tonumber, lua_Number, (lua_State* L, int idx), (L, idx))                                                      \
    X(lua_tonumberx, lua_Number, (lua_State* L, int idx, int* isnum), (L, idx, isnum))                                  \
    X(lua_tointeger, lua_Integer, (lua_State* L, int idx), (L, idx))                                                    \
    X(lua_tointegerx, lua_Integer, (lua_State* L, int idx, int* isnum), (L, idx, isnum))                                \
    X(lua_toboolean, int, (lua_State* L, int idx), (L, idx))                                                            \
    X(lua_tolstring, const char*, (lua_State* L, int idx, size_t* len), (L, idx, len))                                   \
    X(lua_objlen, size_t, (lua_State* L, int idx), (L, idx))                                                            \
    X(lua_tocfunction, lua_CFunction, (lua_State* L, int idx), (L, idx))                                                \
    X(lua_touserdata, void*, (lua_State* L, int idx), (L, idx))                                                         \
    X(lua_tothread, lua_State*, (lua_State* L, int idx), (L, idx))                                                      \
    X(lua_topointer, const void*, (lua_State* L, int idx), (L, idx))                                                    \
    X(lua_pushnil, void, (lua_State* L), (L))                                                                           \
    X(lua_pushnumber, void, (lua_State* L, lua_Number n), (L, n))                                                       \
    X(lua_pushinteger, void, (lua_State* L, lua_Integer n), (L, n))                                                    \
    X(lua_pushlstring, void, (lua_State* L, const char* s, size_t l), (L, s, l))                                       \
    X(lua_pushstring, void, (lua_State* L, const char* s), (L, s))                                                      \
    X(lua_pushvfstring, const char*, (lua_State* L, const char* fmt, va_list argp), (L, fmt, argp))                     \
    X(lua_pushcclosure, void, (lua_State* L, lua_CFunction fn, int n), (L, fn, n))                                     \
    X(lua_pushboolean, void, (lua_State* L, int b), (L, b))                                                            \
    X(lua_pushlightuserdata, void, (lua_State* L, void* p), (L, p))                                                     \
    X(lua_pushthread, int, (lua_State* L), (L))                                                                         \
    X(lua_gettable, void, (lua_State* L, int idx), (L, idx))                                                           \
    X(lua_getfield, void, (lua_State* L, int idx, const char* k), (L, idx, k))                                          \
    X(lua_rawget, void, (lua_State* L, int idx), (L, idx))                                                             \
    X(lua_rawgeti, void, (lua_State* L, int idx, int n), (L, idx, n))                                                  \
    X(lua_createtable, void, (lua_State* L, int narr, int nrec), (L, narr, nrec))                                       \
    X(lua_newuserdata, void*, (lua_State* L, size_t sz), (L, sz))                                                       \
    X(lua_getmetatable, int, (lua_State* L, int objindex), (L, objindex))                                              \
    X(lua_getfenv, void, (lua_State* L, int idx), (L, idx))                                                            \
    X(lua_settable, void, (lua_State* L, int idx), (L, idx))                                                           \
    X(lua_setfield, void, (lua_State* L, int idx, const char* k), (L, idx, k))                                          \
    X(lua_rawset, void, (lua_State* L, int idx), (L, idx))                                                             \
    X(lua_rawseti, void, (lua_State* L, int idx, int n), (L, idx, n))                                                  \
    X(lua_setmetatable, int, (lua_State* L, int objindex), (L, objindex))                                              \
    X(lua_setfenv, int, (lua_State* L, int idx), (L, idx))                                                             \
    X(lua_call, void, (lua_State* L, int nargs, int nresults), (L, nargs, nresults))                                    \
    X(lua_pcall, int, (lua_State* L, int nargs, int nresults, int errfunc), (L, nargs, nresults, errfunc))              \
    X(lua_cpcall, int, (lua_State* L, lua_CFunction func, void* ud), (L, func, ud))                                    \
    X(lua_load, int, (lua_State* L, lua_Reader reader, void* dt, const char* chunkname), (L, reader, dt, chunkname))     \
    X(lua_loadx, int, (lua_State* L, lua_Reader reader, void* dt, const char* chunkname, const char* mode),              \
      (L, reader, dt, chunkname, mode))                                                                                 \
    X(lua_dump, int, (lua_State* L, lua_Writer writer, void* data), (L, writer, data))                                  \
    X(lua_dump_strip, int, (lua_State* L, lua_Writer writer, void* data), (L, writer, data))                            \
    X(lua_yield, int, (lua_State* L, int nresults), (L, nresults))                                                      \
    X(lua_resume, int, (lua_State* L, int narg), (L, narg))                                                            \
    X(lua_status, int, (lua_State* L), (L))                                                                            \
    X(lua_gc, int, (lua_State* L, int what, int data), (L, what, data))                                                \
    X(lua_error, int, (lua_State* L), (L))                                                                             \
    X(lua_next, int, (lua_State* L, int idx), (L, idx))                                                                \
    X(lua_concat, void, (lua_State* L, int n), (L, n))                                                                 \
    X(lua_getallocf, lua_Alloc, (lua_State* L, void** ud), (L, ud))                                                     \
    X(lua_setallocf, void, (lua_State* L, lua_Alloc f, void* ud), (L, f, ud))                                          \
    /* lua_setlevel: declared in lua.h but NOT exported by LuaJIT 2.1 (there is no  */\
    /* such symbol in lua51.dll), so its pointer stays NULL. Nothing in lxe calls */\
    /* it; the stub would abort loudly if anything ever did.                          */\
    X(lua_setlevel, void, (lua_State* from, lua_State* to), (from, to))                                                 \
    X(lua_getstack, int, (lua_State* L, int level, lua_Debug* ar), (L, level, ar))                                       \
    X(lua_getinfo, int, (lua_State* L, const char* what, lua_Debug* ar), (L, what, ar))                                 \
    X(lua_getlocal, const char*, (lua_State* L, const lua_Debug* ar, int n), (L, ar, n))                                 \
    X(lua_setlocal, const char*, (lua_State* L, const lua_Debug* ar, int n), (L, ar, n))                                 \
    X(lua_getupvalue, const char*, (lua_State* L, int funcindex, int n), (L, funcindex, n))                             \
    X(lua_setupvalue, const char*, (lua_State* L, int funcindex, int n), (L, funcindex, n))                             \
    X(lua_sethook, int, (lua_State* L, lua_Hook func, int mask, int count), (L, func, mask, count))                     \
    X(lua_gethook, lua_Hook, (lua_State* L), (L))                                                                      \
    X(lua_gethookmask, int, (lua_State* L), (L))                                                                       \
    X(lua_gethookcount, int, (lua_State* L), (L))                                                                      \
    X(lua_upvalueid, void*, (lua_State* L, int idx, int n), (L, idx, n))                                               \
    X(lua_upvaluejoin, void, (lua_State* L, int idx1, int n1, int idx2, int n2), (L, idx1, n1, idx2, n2))              \
    X(lua_version, const lua_Number*, (lua_State* L), (L))                                                             \
    X(lua_copy, void, (lua_State* L, int fromidx, int toidx), (L, fromidx, toidx))                                     \
    X(lua_isyieldable, int, (lua_State* L), (L))                                                                       \
    /* lauxlib.h */                                                                                                    \
    X(luaL_openlib, void, (lua_State* L, const char* libname, const luaL_Reg* l, int nup), (L, libname, l, nup))           \
    X(luaL_register, void, (lua_State* L, const char* libname, const luaL_Reg* l), (L, libname, l))                      \
    X(luaL_getmetafield, int, (lua_State* L, int obj, const char* e), (L, obj, e))                                      \
    X(luaL_callmeta, int, (lua_State* L, int obj, const char* e), (L, obj, e))                                          \
    X(luaL_typerror, int, (lua_State* L, int narg, const char* tname), (L, narg, tname))                                \
    X(luaL_argerror, int, (lua_State* L, int numarg, const char* extramsg), (L, numarg, extramsg))                      \
    X(luaL_checklstring, const char*, (lua_State* L, int numArg, size_t* l), (L, numArg, l))                            \
    X(luaL_optlstring, const char*, (lua_State* L, int numArg, const char* def, size_t* l), (L, numArg, def, l))        \
    X(luaL_checknumber, lua_Number, (lua_State* L, int numArg), (L, numArg))                                           \
    X(luaL_optnumber, lua_Number, (lua_State* L, int nArg, lua_Number def), (L, nArg, def))                             \
    X(luaL_checkinteger, lua_Integer, (lua_State* L, int numArg), (L, numArg))                                         \
    X(luaL_optinteger, lua_Integer, (lua_State* L, int numArg, lua_Integer def), (L, numArg, def))                      \
    X(luaL_checkstack, void, (lua_State* L, int sz, const char* msg), (L, sz, msg))                                     \
    X(luaL_checktype, void, (lua_State* L, int narg, int t), (L, narg, t))                                             \
    X(luaL_checkany, void, (lua_State* L, int narg), (L, narg))                                                        \
    X(luaL_newmetatable, int, (lua_State* L, const char* tname), (L, tname))                                            \
    X(luaL_checkudata, void*, (lua_State* L, int ud, const char* tname), (L, ud, tname))                                \
    X(luaL_where, void, (lua_State* L, int lvl), (L, lvl))                                                              \
    X(luaL_checkoption, int, (lua_State* L, int narg, const char* def, const char* const lst[]), (L, narg, def, lst))    \
    X(luaL_ref, int, (lua_State* L, int t), (L, t))                                                                     \
    X(luaL_unref, void, (lua_State* L, int t, int ref), (L, t, ref))                                                    \
    X(luaL_loadfile, int, (lua_State* L, const char* filename), (L, filename))                                          \
    X(luaL_loadbuffer, int, (lua_State* L, const char* buff, size_t sz, const char* name), (L, buff, sz, name))           \
    X(luaL_loadstring, int, (lua_State* L, const char* s), (L, s))                                                      \
    X(luaL_newstate, lua_State*, (void), ())                                                                            \
    X(luaL_gsub, const char*, (lua_State* L, const char* s, const char* p, const char* r), (L, s, p, r))                \
    X(luaL_findtable, const char*, (lua_State* L, int idx, const char* fname, int szhint), (L, idx, fname, szhint))      \
    X(luaL_fileresult, int, (lua_State* L, int stat, const char* fname), (L, stat, fname))                              \
    X(luaL_execresult, int, (lua_State* L, int stat), (L, stat))                                                        \
    X(luaL_loadfilex, int, (lua_State* L, const char* filename, const char* mode), (L, filename, mode))                  \
    X(luaL_loadbufferx, int, (lua_State* L, const char* buff, size_t sz, const char* name, const char* mode),              \
      (L, buff, sz, name, mode))                                                                                         \
    X(luaL_traceback, void, (lua_State* L, lua_State* L1, const char* msg, int level), (L, L1, msg, level))               \
    X(luaL_setfuncs, void, (lua_State* L, const luaL_Reg* l, int nup), (L, l, nup))                                    \
    X(luaL_pushmodule, void, (lua_State* L, const char* modname, int sizehint), (L, modname, sizehint))                  \
    X(luaL_testudata, void*, (lua_State* L, int ud, const char* tname), (L, ud, tname))                                \
    X(luaL_setmetatable, void, (lua_State* L, const char* tname), (L, tname))                                          \
    X(luaL_buffinit, void, (lua_State* L, luaL_Buffer* B), (L, B))                                                     \
    X(luaL_prepbuffer, char*, (luaL_Buffer* B), (B))                                                                    \
    X(luaL_addlstring, void, (luaL_Buffer* B, const char* s, size_t l), (B, s, l))                                     \
    X(luaL_addstring, void, (luaL_Buffer* B, const char* s), (B, s))                                                   \
    X(luaL_addvalue, void, (luaL_Buffer* B), (B))                                                                      \
    X(luaL_pushresult, void, (luaL_Buffer* B), (B))                                                                     \
    /* lualib.h */                                                                                                     \
    X(luaopen_base, int, (lua_State* L), (L))                                                                           \
    X(luaopen_math, int, (lua_State* L), (L))                                                                           \
    X(luaopen_string, int, (lua_State* L), (L))                                                                         \
    X(luaopen_table, int, (lua_State* L), (L))                                                                          \
    X(luaopen_io, int, (lua_State* L), (L))                                                                             \
    X(luaopen_os, int, (lua_State* L), (L))                                                                             \
    X(luaopen_package, int, (lua_State* L), (L))                                                                        \
    X(luaopen_debug, int, (lua_State* L), (L))                                                                          \
    X(luaopen_bit, int, (lua_State* L), (L))                                                                            \
    X(luaopen_jit, int, (lua_State* L), (L))                                                                            \
    X(luaopen_ffi, int, (lua_State* L), (L))                                                                            \
    X(luaopen_string_buffer, int, (lua_State* L), (L))                                                                  \
    X(luaL_openlibs, void, (lua_State* L), (L))                                                                         \
    /* luajit.h */                                                                                                     \
    X(luaJIT_setmode, int, (lua_State* L, int idx, int mode), (L, idx, mode))                                           \
    X(luaJIT_profile_start, void, (lua_State* L, const char* mode, luaJIT_profile_callback cb, void* data),              \
      (L, mode, cb, data))                                                                                              \
    X(luaJIT_profile_stop, void, (lua_State* L), (L))                                                                    \
    X(luaJIT_profile_dumpstack, const char*, (lua_State* L, const char* fmt, int depth, size_t* len), (L, fmt, depth, len)) \
    X(luaJIT_version_2_1_0_beta3, void, (void), ())                                                                     \
    /* LuaXE's patched LuaJIT (lib_package.c / lj_load.c) - absent from stock LuaJIT */                                  \
    X(ll_loadfunc, int, (lua_State* L, const char* path, const char* name, int r), (L, path, name, r))

// The symbols lxe cannot run without. A DLL missing any REQUIRED export is
// refused whatever its hash says: it is a different LuaJIT build.
#define LUA_RUNTIME_REQUIRED(X)                                                                                         \
    X(luaL_newstate)                                                                                                   \
    X(lua_close)                                                                                                        \
    X(lua_pushstring)                                                                                                   \
    X(lua_pushcclosure)                                                                                                 \
    X(luaL_openlibs)                                                                                                   \
    X(luaopen_ffi)                                                                                                     \
    X(luaL_loadbuffer)                                                                                                  \
    X(lua_tolstring)                                                                                                    \
    X(lua_gettop)                                                                                                       \
    X(lua_settop)                                                                                                       \
    X(lua_setfield)                                                                                                     \
    X(lua_getfield)                                                                                                     \
    X(lua_pcall)                                                                                                        \
    X(lua_rawgeti)                                                                                                      \
    X(lua_rawseti)                                                                                                      \
    X(ll_loadfunc)                                                                                                      \
    X(lua_dump_strip)

// Columns of LUA_RUNTIME_FUNCTIONS: name, return type, parameter types,
// argument names. Each macro below takes them in that same order.
#define LUA_RT_POINTER(name, ret, params, args) static ret (*p_##name) params = nullptr;

LUA_RUNTIME_FUNCTIONS(LUA_RT_POINTER)

/// Called by every stub whose pointer is still null: a Lua call before the
/// runtime is bound must fail loudly, never silently.
[[noreturn]] static void lua_runtime_unbound(const char* name) {
    std::cerr << "LuaXE: the Lua runtime is not loaded; " << name
              << " cannot run. This is a bug: lua_runtime::ensure() must succeed before any Lua call."
              << std::endl;
    std::abort();
}

#define LUA_RT_STUB(name, ret, params, args)                                                                             \
    extern "C" ret name params {                                                                                        \
        if (!p_##name) lua_runtime_unbound(#name);                                                                     \
        return p_##name args;                                                                                           \
    }

LUA_RUNTIME_FUNCTIONS(LUA_RT_STUB)

// Two varargs entry points cannot be forwarded, so they are built on the va_list
// ones. These are LuaJIT's own implementations: lua_pushfstring is lj_api.c
// (lj_gc_check + lj_strfmt_pushvf), luaL_error is lj_err.c (lj_strfmt_pushvf +
// lj_err_callermsg = luaL_where + concat + lua_error).
extern "C" const char* lua_pushfstring(lua_State* L, const char* fmt, ...) {
    if (!p_lua_pushvfstring) lua_runtime_unbound("lua_pushvfstring");
    va_list argp;
    va_start(argp, fmt);
    const char* result = p_lua_pushvfstring(L, fmt, argp);
    va_end(argp);
    return result;
}

extern "C" int luaL_error(lua_State* L, const char* fmt, ...) {
    if (!p_lua_pushvfstring) lua_runtime_unbound("lua_pushvfstring");
    if (!p_luaL_where) lua_runtime_unbound("luaL_where");
    if (!p_lua_concat) lua_runtime_unbound("lua_concat");
    if (!p_lua_error) lua_runtime_unbound("lua_error");
    va_list argp;
    va_start(argp, fmt);
    luaL_where(L, 1);
    lua_pushvfstring(L, fmt, argp);
    va_end(argp);
    lua_concat(L, 2);
    return lua_error(L);
}

namespace {

HMODULE g_module = nullptr;
std::string g_path;

// GetProcAddress returns FARPROC, so each pointer is resolved through its own
// declared type (decltype(&name)) - writing `ret(*)(params)` in a cast would be
// parsed as a function RETURNING a function pointer.
#define LUA_RT_BIND(name, ret, params, args) p_##name = resolve<decltype(&name)>(module, #name);
#define LUA_RT_RESET(name, ret, params, args) p_##name = nullptr;
// LUA_RUNTIME_REQUIRED passes names only, the table passes name + signature.
#define LUA_RT_MISSING(name, ...) if (!p_##name) missing.push_back(#name);

template <typename Fn>
Fn resolve(HMODULE module, const char* name) {
    return reinterpret_cast<Fn>(GetProcAddress(module, name));
}

/// Forget every bound pointer: used before a bind and when one is abandoned, so
/// no half-bound table can survive into the next attempt.
void reset_bind() {
    LUA_RUNTIME_FUNCTIONS(LUA_RT_RESET)
    g_module = nullptr;
    g_path.clear();
}

// ---------------------------------------------------------------------------
// Known-good runtimes. luaxe.dev publishes no hash manifest, so the two builds
// of this patched LuaJIT are pinned here. Any other hash is refused unless
// LUAXE_ALLOW_UNVERIFIED_LUA51 says otherwise.
// ---------------------------------------------------------------------------
const char* kKnownGood[] = {
    // lua51.dll as served by luaxe.dev (LuaJIT 2.1.M.64, patched: ll_loadfunc)
    "3add53a27543bfccf5eb355eeb5499e2a7ea61f7624cac539b735ba6056178f9",
    // the same patched LuaJIT built in C:\LuaJIT-2.1.M.64\src
    "554caf2a2ca3e1ae4b32ed2c1bc146eac73f8a6fe77609a15fe2d19fc64bf46e",
};

bool truthy_env(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text == "1" || text == "true" || text == "on" || text == "yes";
}

std::filesystem::path exe_dir() {
    // A path longer than MAX_PATH must not silently become the working directory:
    // that would let the CWD choose which runtime gets loaded. Fail loudly.
    std::wstring buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
    if (!length) return std::filesystem::current_path();
    if (length == buffer.size()) return std::filesystem::current_path(); // truncated
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

bool bind(HMODULE module, std::string* error) {
    // A failed bind must not leave half a table behind: unset pointers plus
    // loaded()==false is a trap for the next caller.
    reset_bind();
    LUA_RUNTIME_FUNCTIONS(LUA_RT_BIND)
    std::vector<const char*> missing;
    LUA_RUNTIME_REQUIRED(LUA_RT_MISSING)
    if (!missing.empty()) {
        std::string names;
        for (const char* name : missing) {
            if (!names.empty()) names += ", ";
            names += name;
        }
        if (error) *error = "not a usable LuaXE runtime: it does not export " + names
            + " (ll_loadfunc/lua_dump_strip mark LuaXE's patched LuaJIT; a stock LuaJIT build is not usable)";
        return false;
    }
    g_module = module;
    wchar_t full[MAX_PATH];
    g_path = GetModuleFileNameW(module, full, MAX_PATH) ? std::filesystem::path(full).string() : "<unknown>";
    // TB-195 / luajit.h:76: this symbol exists to be "called from main" so a
    // version mismatch is a named failure here instead of a mystery later. It is
    // a no-op when the version matches.
    if (p_luaJIT_version_2_1_0_beta3) p_luaJIT_version_2_1_0_beta3();
    return true;
}

bool load_file(const std::filesystem::path& file, std::string* error) {
    std::string reason;
    if (!lua_runtime::verify(file, &reason)) {
        if (error) *error = reason;
        return false;
    }
    // Keep the hash the file had when it was verified: verify() reads the path,
    // LoadLibraryExW maps the path, and anything able to write beside the
    // executable could swap the file in between. The MAPPED image is re-hashed
    // below, so the check cannot be raced past.
    auto verified_hash = lua_runtime::sha256(file);
    // LOAD_WITH_ALTERED_SEARCH_PATH alone: the runtime's own directory is its search
// path, so lua51.dll's dependencies (VCRUNTIME140.dll) resolve from beside it.
// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR cannot be combined with it (Windows error 87).
HMODULE module = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        if (error) *error = "could not load " + file.string() + " (Windows error "
            + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!bind(module, error)) {
        FreeLibrary(module);
        return false;
    }
    // Re-hash what is ACTUALLY mapped. A control whose whole job is "never load
    // an unverified binary" should not race its own check; nothing has run Lua
    // yet (bind happens before the first lua_State exists), so releasing the
    // image here is safe.
    {
        wchar_t mapped[MAX_PATH];
        auto mapped_hash = GetModuleFileNameW(module, mapped, MAX_PATH)
            ? lua_runtime::sha256(std::filesystem::path(mapped)) : std::string();
        if (!verified_hash.empty() && mapped_hash != verified_hash) {
            if (error) *error = "refused " + file.string()
                + ": the file changed between verification and loading (verified " + verified_hash
                + ", loaded " + (mapped_hash.empty() ? std::string("<unreadable>") : mapped_hash) + ")";
            FreeLibrary(module);
            reset_bind();
            return false;
        }
    }
    if (truthy_env("LUAXE_RUNTIME_VERBOSE")) {
        std::cerr << "[runtime] " << lua_runtime::diagnostics() << " (" << reason << ")" << std::endl;
    }
    return true;
}

// Download into the executable's own modules directory, never the working
// directory: for a project run the working directory is a source tree (TB-190).
//
// Atomic: the download lands on lua51.dll.<pid>.tmp, is verified THERE, and only
// then replaces the final file. Two lxe processes starting cold therefore never
// hand a half-written DLL to the loader or to each other's hash check, and a
// failed or unverified download leaves nothing behind but the temp file.
bool download(std::string* error) {
    std::filesystem::path target = exe_dir() / "modules" / lua_runtime::kDllName;
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        if (error) *error = "could not create " + target.parent_path().string() + " (" + ec.message()
            + "): the Lua runtime has to be cached somewhere writable";
        return false;
    }
    auto temp = target;
    temp += "." + std::to_string(GetCurrentProcessId()) + ".tmp";
    std::filesystem::remove(temp, ec);
    API api;
    // API::DownloadFile parses Content-Length and can throw on a response that
    // has none; a download failure is a normal outcome here, not a crash.
    bool ok = false;
    try {
        ok = api.DownloadFile("luaxe.dev", "/module/lua51.dll", temp.string());
    } catch (const std::exception& why) {
        std::filesystem::remove(temp, ec);
        if (error) *error = "downloading the Lua runtime from luaxe.dev failed: " + std::string(why.what());
        return false;
    }
    if (!ok) {
        std::filesystem::remove(temp, ec);
        if (error) *error = "could not download the Lua runtime from luaxe.dev/module/lua51.dll into "
            + target.string();
        return false;
    }
    if (!load_file(temp, error)) {
        std::filesystem::remove(temp, ec); // never keep an unverified download
        return false;
    }
    // Bound and correct: publish it under the name the next start looks for.
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (error) *error = "could not install the verified Lua runtime at " + target.string()
            + " (Windows error " + std::to_string(GetLastError()) + "); it is loaded from "
            + lua_runtime::path();
        return true; // the loaded image is good; the cache is just not there
    }
    return true;
}

} // namespace

namespace lua_runtime {

std::string sha256(const std::filesystem::path& file) {
    // Windows CNG (bcrypt.dll), which is present on every supported Windows: less
    // code of ours to trust in the one place that decides whether a binary gets
    // executed. Read in chunks so a large runtime never has to be in memory.
    std::ifstream input(file, std::ios::binary);
    if (!input) return "";
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    DWORD object_size = 0, result_size = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size, sizeof(object_size),
            &result_size, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return "";
    }
    std::vector<unsigned char> object(object_size);
    BCRYPT_HASH_HANDLE hash = nullptr;
    // This SDK's signature is (hAlgorithm, phHash, pbHashObject, cbHashObject,
    // pbSecret, cbSecret, dwFlags) - the hash handle is the SECOND argument.
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return "";
    }
    std::vector<char> buffer(65536);
    bool ok = true;
    while (ok) {
        input.read(buffer.data(), (std::streamsize)buffer.size());
        auto read = input.gcount();
        if (read > 0 && BCryptHashData(hash, (PUCHAR)buffer.data(), (ULONG)read, 0) != 0) ok = false;
        if (read <= 0) break;
    }
    unsigned char digest[32] = { 0 };
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return "";
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (auto byte : digest) {
        out += hex[byte >> 4];
        out += hex[byte & 0xF];
    }
    return out;
}

bool verify(const std::filesystem::path& file, std::string* reason) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        if (reason) *reason = "no such file: " + file.string();
        return false;
    }
    auto hash = sha256(file);
    if (hash.empty()) {
        if (reason) *reason = "could not read " + file.string();
        return false;
    }
    bool known = false;
    for (auto candidate : kKnownGood) known = known || hash == candidate;
    if (!known && !truthy_env("LUAXE_ALLOW_UNVERIFIED_LUA51")) {
        if (reason) *reason = "sha256 " + hash + " of " + file.string()
            + " is not a known-good LuaXE runtime. Add the hash to kKnownGood in src/lua/lua_runtime.cpp,"
            + " or set LUAXE_ALLOW_UNVERIFIED_LUA51=1 to load it anyway";
        return false;
    }
    if (reason) *reason = "sha256 " + hash + (known ? " (known-good build)" : " (UNVERIFIED, allowed by env)");
    return true;
}

bool known_good(const std::filesystem::path& file) {
    auto hash = sha256(file);
    if (hash.empty()) return false;
    for (auto candidate : kKnownGood) {
        if (hash == candidate) return true;
    }
    return false;
}

bool ensure(const std::filesystem::path& bundled_dir, std::string* error) {
    if (loaded()) return true;

    // Someone already loaded lua51.dll (a module, or the payload of a second
    // copy of this executable): bind to THAT image, never load a second one.
    if (HMODULE existing = GetModuleHandleW(lua_runtime::kDllName)) {
        if (bind(existing, error)) return true;
        if (error) *error += " (it was already loaded by another module)";
        return false;
    }

    const auto beside = exe_dir();
    std::vector<std::filesystem::path> candidates = {
        beside / lua_runtime::kDllName,
        beside / "modules" / lua_runtime::kDllName,
    };
    if (!bundled_dir.empty()) {
        // An explicit extra directory (no caller passes one today: a payload's
        // runtime is unpacked into <exe>\modules, the second candidate above).
        // Both shapes are tried.
        candidates.push_back(bundled_dir / lua_runtime::kDllName);
        candidates.push_back(bundled_dir / "modules" / lua_runtime::kDllName);
    }

    std::vector<std::string> refusals;
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) continue;
        std::string why;
        if (load_file(candidate, &why)) return true;
        refusals.push_back(why);
    }
    // A refused or broken local copy never blocks a fresh download.
    std::string downloaded_error;
    if (download(&downloaded_error)) return true;
    if (error) {
        *error = downloaded_error.empty() ? "no usable Lua runtime" : downloaded_error;
        for (const auto& refusal : refusals) *error = refusal + "; " + *error;
    }
    return false;
}

bool loaded() { return g_module != nullptr; }

const std::string& path() { return g_path; }

std::string diagnostics() {
    if (!loaded()) return "Lua runtime: not loaded in this process (it runs no Lua)";
    char text[1024];
    std::snprintf(text, sizeof(text), "Lua runtime: %s @ %p (lua_close -> %p, ll_loadfunc -> %p)",
        g_path.c_str(), (void*)g_module, (void*)p_lua_close, (void*)p_ll_loadfunc);
    return text;
}

} // namespace lua_runtime