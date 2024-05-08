#pragma once

#include <string>
#include <lua.hpp>

class API {
public:
	std::string WebRequest(const std::wstring& Host, const std::string UrlPath, const std::string prefix = "GET", const std::string header = "", const std::string suffix = "");

	bool WorkOnBody(std::string& Body, std::string& ChunkedBody, int& CurrentChunkSize);

    bool DownloadFile(std::string Host, std::string UrlPath, std::string FilePath);
};