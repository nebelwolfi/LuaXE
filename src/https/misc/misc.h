#pragma once

#ifndef MISC_H
#define MISC_H

#include <string>

std::string utf8_encode(const std::wstring &wstr);
std::wstring utf8_decode(const std::string &str);
#endif
