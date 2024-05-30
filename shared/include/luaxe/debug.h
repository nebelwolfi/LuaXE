#ifndef LUAXE_DEBUG_H
#define LUAXE_DEBUG_H

namespace lua::debug {
    namespace detail {
        static void dump_table(lua_State *L, int idx, decltype(printf) printfun, const char *tabs = "") {
            idx = lua_absindex(L, idx);
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                int t = lua_type(L, -1);
                switch (t) {
                    case LUA_TTABLE:
                        if (lua_type(L, -2) == LUA_TNUMBER)
                            printfun("%s[%d] = {\n", tabs, lua_tointeger(L, -2));
                        else
                            printfun("%s[%s] = {\n", tabs, lua_tostring(L, -2));
                        if (strlen(tabs) < 4)
                            dump_table(L, -1, printfun, (std::string(tabs) + "\t").c_str());
                        printfun("%s}\n", tabs);
                        break;
                    case LUA_TSTRING:  /* strings */
                        if (lua_type(L, -2) == LUA_TNUMBER)
                            printfun("%s[%d] = '%s'\n", tabs, lua_tointeger(L, -2), lua_tostring(L, -1));
                        else
                            printfun("%s[%s] = '%s'\n", tabs, lua_tostring(L, -2), lua_tostring(L, -1));
                        break;
                    case LUA_TBOOLEAN:  /* booleans */
                        if (lua_type(L, -2) == LUA_TNUMBER)
                            printfun("%s[%d] = %s\n", tabs, lua_tointeger(L, -2), lua_toboolean(L, -1) ? "true" : "false");
                        else
                            printfun("%s[%s] = %s\n", tabs, lua_tostring(L, -2), lua_toboolean(L, -1) ? "true" : "false");
                        break;
                    case LUA_TNUMBER:  /* numbers */
                        if (lua_type(L, -2) == LUA_TNUMBER)
                            printfun("%s[%d] = %g\n", tabs, lua_tointeger(L, -2), lua_tonumber(L, -1));
                        else
                            printfun("%s[%s] = %g\n", tabs, lua_tostring(L, -2), lua_tonumber(L, -1));
                        break;
                    default:  /* other values */
                        if (lua_type(L, -2) == LUA_TNUMBER)
                            printfun("%s[%d] = %s\n", tabs, lua_tointeger(L, -2), lua_typename(L, lua_type(L, -1)));
                        else
                            printfun("%s[%s] = %s\n", tabs, lua_tostring(L, -2), lua_typename(L, lua_type(L, -1)));
                        break;
                }
                lua_pop(L, 1);
            }
        }
    }
    static void dump(lua_State *L, decltype(printf) printfun = printf)
    {
        int i;
        int top = lua_gettop(L);
        for (i = 1; i <= top; i++) {  /* repeat for each level */
            int t = lua_type(L, i);
            switch (t) {
                case LUA_TTABLE:  /* table */
                    printfun("[%d] = {\n", i);
                    detail::dump_table(L, i, printfun, "\t");
                    printfun("}\n");
                    break;
                case LUA_TSTRING:  /* strings */
                    printfun("[%d] = '%s'\n", i, lua_tostring(L, i));
                    break;
                case LUA_TBOOLEAN:  /* booleans */
                    printfun("[%d] = %s\n", i, lua_toboolean(L, i) ? "true" : "false");
                    break;
                case LUA_TNUMBER:  /* numbers */
                    if (lua_tonumber(L, i) > 0x100000 && lua_tonumber(L, i) < 0xffffffff)
                        printfun("[%d] = %X\n", i, (uint32_t)lua_tonumber(L, i));
                    else
                        printfun("[%d] = %g\n", i, lua_tonumber(L, i));
                    break;
                case LUA_TUSERDATA:  /* numbers */
                    printfun("[%d] = %s %llX\n", i, lua_typename(L, t), (uintptr_t)lua_touserdata(L, i));
                    break;
                default:  /* other values */
                    printfun("[%d] = %s\n", i, lua_typename(L, t));
                    break;
            }
        }
    }
    static void trace(lua_State* L, decltype(printf) printfun)
    {
        luaL_traceback(L, L, NULL, 1);
        const char* lerr = lua_tostring(L, -1); // -0
        if (strlen(lerr) > 0) {
            printfun(lerr);
        }
        lua_pop(L, 1);
    }
}

#endif //LUAXE_DEBUG_H
