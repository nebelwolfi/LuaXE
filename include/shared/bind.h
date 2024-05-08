//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_BIND_H
#define LUAXE_BIND_H

inline int lua_absindex (lua_State* L, int idx)
{
    if (idx > LUA_REGISTRYINDEX && idx < 0)
        return lua_gettop (L) + idx + 1;
    else
        return idx;
}
inline int lua_rawgetp (lua_State* L, int idx, void const* p)
{
    idx = lua_absindex (L, idx);
    lua_pushlightuserdata (L, const_cast <void*> (p));
    lua_rawget (L,idx);
    return lua_type(L, -1);
}
inline void lua_rawsetp (lua_State* L, int idx, void const* p)
{
    idx = lua_absindex (L, idx);
    lua_pushlightuserdata (L, const_cast <void*> (p));
    // put key behind value
    lua_insert (L, -2);
    lua_rawset (L, idx);
}
inline int luaL_gettable(lua_State *L, int idx) {
    lua_gettable(L, idx);
    return lua_type(L, -1);
}
inline int luaL_getfield(lua_State *L, int idx, const char* name) {
    lua_getfield(L, idx, name);
    return lua_type(L, -1);
}

namespace lua::bind {
namespace detail {
template<typename T>
struct bindable {
    template<class T>
    class metatable_helper {
    public:
        static void const* key() { static char value; return &value; }
        static bool push_metatable(lua_State * L) {
            lua_getfield(L, LUA_REGISTRYINDEX, "__METASTORE");
            if (lua_rawgetp(L, -1, key()))
            {
                lua_remove(L, -2);
                return false;
            }
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_rawsetp(L, -3, key());
            lua_remove(L, -2);
            return true;
        }
    };
    lua_State* L = nullptr;
    bindable(lua_State* L) : L(L) {}
    bool push_metatable()
    {
        return metatable_helper<std::remove_pointer_t<std::decay_t<T>>>::push_metatable(L);
    }
    void push_function_index()
    {
        push_metatable();
        lua_pushstring(L, "__findex");
        if (!luaL_gettable(L, -2))
        {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, "__findex");
        }
        lua_remove(L, -2);
    }
    void push_property_index()
    {
        push_metatable();
        lua_pushstring(L, "__pindex");
        if (!luaL_gettable(L, -2))
        {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, "__pindex");
        }
        lua_remove(L, -2);
    }
    void push_property_newindex()
    {
        push_metatable();
        lua_pushstring(L, "__pnewindex");
        if (!luaL_gettable(L, -2))
        {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, "__pnewindex");
        }
        lua_remove(L, -2);
    }
    bindable<T>& fun(const char* name, lua_CFunction f)
    {
        push_function_index();
        lua_pushcfunction(L, f);
        lua_setfield(L, -2, name);
        lua_pop(L, 1);
        return *this;
    }
    bindable<T>& meta_fun(const char* name, lua_CFunction f)
    {
        if (strcmp(name, "__gc") == 0) {
            return meta_fun("__cgc", f);
        }
        push_metatable();
        lua_pushcfunction(L, f);
        lua_setfield(L, -2, name);
        lua_pop(L, 1);
        return *this;
    }
    bindable<T>& prop(const char* name, lua_CFunction findex, lua_CFunction fnewindex = nullptr)
    {
        push_property_index();
        lua_pushcfunction(L, findex);
        lua_setfield(L, -2, name);
        lua_pop(L, 1);
        if (!fnewindex) return *this;
        push_property_newindex();
        lua_pushcfunction(L, fnewindex);
        lua_setfield(L, -2, name);
        lua_pop(L, 1);
        return *this;
    }
    const char* type_name()
    {
        push_metatable();
        if (luaL_getfield(L, -1, "__name"))
        {
            auto name = lua_tostring(L, -1);
            lua_pop(L, 2);
            return name;
        }
        lua_pop(L, 2);
        return "userdata";
    }
    bool check(int idx)
    {
        if (!lua_isuserdata(L, idx))
            return false;
        lua_getmetatable(L, idx);
        push_metatable();
        auto result = lua_rawequal(L, -1, -2);
        lua_pop(L, 2);
        return result;
    }
};
static int lua_CIndexFunction(lua_State* L)
{
    if (luaL_getmetafield(L, 1, "__pindex"))
    {
        lua_pushvalue(L, 2);
        if (luaL_gettable(L, -2))
        {
            lua_remove(L, -2);
            lua_insert(L, 1);
            lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
            return lua_gettop(L);
        }
        else lua_pop(L, 1);
    }
    if (luaL_getmetafield(L, 1, "__findex"))
    {
        lua_pushvalue(L, 2);
        if (luaL_gettable(L, -2))
            return 1;
        else lua_pop(L, 1);
    }
    if (luaL_getmetafield(L, 1, lua_tostring(L, 2)))
        return 1;
    if (luaL_getmetafield(L, 1, "__cindex"))
    {
        lua_insert(L, 1);
        lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
        return lua_gettop(L);
    }
    return 0;
}
static int lua_CNewIndexFunction(lua_State* L)
{
    if (luaL_getmetafield(L, 1, "__pnewindex"))
    {
        lua_pushvalue(L, 2);
        if (luaL_gettable(L, -2))
        {
            lua_pushvalue(L, 1);
            lua_pushvalue(L, 2);
            lua_pushvalue(L, 3);
            lua_call(L, 3, 0);
            return 0;
        }
        else lua_pop(L, 1);
    }
    if (luaL_getmetafield(L, 1, "__cnewindex"))
    {
        lua_pushvalue(L, 1);
        lua_pushvalue(L, 2);
        lua_pushvalue(L, 3);
        lua_call(L, 3, 0);
        return 0;
    }
    return 0;
}
static int lua_EQFunction(lua_State* L)
{
    if (!lua_isuserdata(L, 1) || !lua_isuserdata(L, 2))
        lua_pushboolean(L, false);
    else
        lua_pushboolean(L, *(uintptr_t*)lua_touserdata(L, 1) == *(uintptr_t*)lua_touserdata(L, 2));
    return 1;
}
static int lua_CGCFunction(lua_State* L)
{
    if (luaL_getmetafield(L, 1, "__cgc"))
    {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0))
            return lua_error(L);
    }
    auto ud = lua_touserdata(L, 1);
    if (*(bool*)((uintptr_t*)ud+1)) {
        free((void*)*(uintptr_t*)ud);
    }
    return 0;
}
static void* topointer(lua_State *L, int idx)
{
    return *(void**)lua_touserdata(L, idx);
}
static int tostring(lua_State* L)
{
    char asdf[128] = { 0 };
    snprintf(asdf, sizeof(asdf), "%s{%llX}", lua_tostring(L, lua_upvalueindex(1)), (uintptr_t)topointer(L, 1));
    lua_pushstring(L, asdf);
    return 1;
}
}
template<typename T>
static detail::bindable<T> add(lua_State* L, const std::string& name) {
    auto b = detail::bindable<T>(L);
    if (!b.push_metatable()) {
        lua_pop(L, 1);
        return b;
    }
    lua_pushstring(L, name.c_str());
    lua_setfield(L, -2, "__name");

    lua_pushstring(L, name.c_str());
    lua_pushcclosure(L, detail::tostring, 1);
    lua_setfield(L, -2, "__tostring");

    lua_pushcclosure(L, detail::lua_CIndexFunction, 0);
    lua_setfield(L, -2, "__index");

    lua_pushcclosure(L, detail::lua_CNewIndexFunction, 0);
    lua_setfield(L, -2, "__newindex");

    lua_pushcclosure(L, detail::lua_EQFunction, 0);
    lua_setfield(L, -2, "__eq");

    lua_pushcclosure(L, detail::lua_CGCFunction, 0);
    lua_setfield(L, -2, "__gc");

    lua_pop(L, 1);
    return b;
}
}

namespace lua {
template<typename T>
static T* push(lua_State* L, T* p, bool gc = false) {
    auto ud = lua_newuserdata(L, sizeof(uintptr_t) + sizeof(bool));
    *(uintptr_t*)ud = (uintptr_t)p;
    *(bool*)((uintptr_t*)ud+1) = gc;
    bind::detail::bindable<T>(L).push_metatable();
    lua_setmetatable(L, -2);
    return p;
}
template<typename T>
static T* alloc(lua_State* L) {
    return push<T>(L, (T*)malloc(sizeof(T)), true);
}
template<typename T>
static T* check(lua_State* L, int idx) {
    if (!bind::detail::bindable<T>(L).check(idx))
        luaL_typerror(L, 1, lua::bind::detail::bindable<T>(L).type_name());
    return *(T**)lua_touserdata(L, idx);
}

static int tack_on_traceback(lua_State* L) {
    const char * msg = lua_tostring(L, -1);
    luaL_traceback(L, L, msg, 2);
    lua_remove(L, -2);
    return 1;
}
static int pcall(lua_State* L, int nargs, int nresults)
{
    int errindex = lua_gettop(L) - nargs;
    lua_pushcclosure(L, tack_on_traceback, 0);
    lua_insert(L, errindex);
    auto status = lua_pcall(L, nargs, nresults, errindex);
    lua_remove(L, errindex);
    return status;
}
}


#endif //LUAXE_BIND_H