#pragma once
#define VERSION_MAJOR 1
#define VERSION_MINOR 0
#define VERSION_PATCH 0
#define VERSION_BUILD 311

#define __stringify__(a) __stringify___(a)
#define __stringify___(a) #a
#define LuaXE_VERSION __stringify__(VERSION_MAJOR) "." __stringify__(VERSION_MINOR) "." __stringify__(VERSION_PATCH) "." __stringify__(VERSION_BUILD)
