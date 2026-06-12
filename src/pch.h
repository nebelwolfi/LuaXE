#pragma once

#define WIN32_LEAN_AND_MEAN
#define _MFC_OVERRIDES_NEW
#define NOMINMAX
#define _USE_MATH_DEFINES
#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <windowsx.h>

#include <wrl/client.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <wincodec.h>
#include <WS2tcpip.h>
#include <span>
#include <mutex>

#include <luaxe/luaxe.hpp>