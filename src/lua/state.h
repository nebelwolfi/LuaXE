//
// Created by Nebelwolfi on 08/05/2024.
//

#ifndef LUAXE_STATE_H
#define LUAXE_STATE_H

void load_lua_state_and_run(std::function<void(lua_State*)> func, bool compiled);
void load_lua_file(lua_State* L, const std::string& source);
void load_lua_memory(lua_State* L, const std::string& source, const std::string& chunk_name);
void load_lef_file(lua_State* L, const std::string& path);
void load_lef_memory(lua_State* L, const std::string& data);

#endif //LUAXE_STATE_H
