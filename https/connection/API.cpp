#include "API.h"

#include "../socket/framework.h"
#include "../socket/ActiveSock.h"
#include "../socket/SSLClient.h"
#include "../socket/EventWrapper.h"
#include "../socket/CertHelper.h"
#include <fstream>
#include "../misc/misc.h"

#include <codecvt>
#include <filesystem>

std::string API::WebRequest(const std::wstring& Host, const std::string UrlPath, const std::string prefix, const std::string header, const std::string suffix) {
	int Port = 443;

    std::string HostA = utf8_encode(Host);

	CEventWrapper ShutDownEvent;
	auto pActiveSock = std::make_unique<CActiveSock>(ShutDownEvent);
	pActiveSock->SetRecvTimeoutSeconds(30);
	pActiveSock->SetSendTimeoutSeconds(60);

    //printf("Connecting to %s:%d", HostA.c_str(), Port);
	bool b = pActiveSock->Connect(Host.c_str(), static_cast<USHORT>(Port));
	if (!b) {
		printf("Could not connect to the Server\n");
		return "";
	}
    //printf("Connected to the Server");

	auto pSSLClient = std::make_unique<CSSLClient>(pActiveSock.get());
	pSSLClient->ServerCertAcceptable = CertAcceptable;
	pSSLClient->SelectClientCertificate = SelectClientCertificate;
	HRESULT hr = pSSLClient->Initialize(Host.c_str());
	if (!SUCCEEDED(hr)) {
        printf("Failed to Init SSL %llX\n", hr);
		return "";
	}
    //printf("SSL Initialized");

	std::string RequestString = prefix + " " + UrlPath + " HTTP/1.1\r\nAccept: text/html\r\nAccept-Encoding: none\r\nAccept-Language: en-US;q=0.8,en;q=0.7\r\nCache-Control: no-cache\r\nHost: " + HostA + "\r\n" + header;
    if (suffix.empty())
        RequestString += "\r\n\r\n";
    else {
        RequestString += "\r\nContent-Length: " + std::to_string(suffix.size()) + "\r\n\r\n" + suffix;
    }

    //printf("HTTPS Sent Request %s\n", RequestString.c_str());

	pSSLClient->Send(RequestString.c_str(), RequestString.size());

    //printf("HTTPS Sent Request %s\n", UrlPath.c_str());

    int BufferBytesReceiving = 4096;
    int ContentLength = 0;
    bool GotHeaders = false;
    bool RequestFinished = false;
    bool IsChunked = false;
    bool AlreadyAddedNewMsg = false;
    int CurrentChunkSize = 0;
    std::string CompleteReceive = "";
    std::string Header = "";
    std::string Body = "";
    std::string ChunkedBody = "";

	while (!RequestFinished) {
		std::string ReceiveMsgBuffer;
		ReceiveMsgBuffer.resize(BufferBytesReceiving);
		ReceiveMsgBuffer.reserve(BufferBytesReceiving);
		int BytesReceived = 0;

		int res = 1;
		res = (BytesReceived = pSSLClient->Recv(&ReceiveMsgBuffer[0], BufferBytesReceiving));
		if (0 < res) {
			std::string ReceiveMsg = ReceiveMsgBuffer.substr(0, BytesReceived);
            //printf("%s", ReceiveMsg.c_str());
			//std::cout << "ReceiveMsg: " << ReceiveMsg << std::endl;
			CompleteReceive += ReceiveMsg;
			if (!GotHeaders && CompleteReceive.find("\r\n\r\n") != std::string::npos) {
				Header = CompleteReceive.substr(0, CompleteReceive.find("\r\n\r\n"));
				Body = CompleteReceive.substr(CompleteReceive.find("\r\n\r\n") + 4);
				GotHeaders = true;
				//printf("Got Headers");
				//printf(Header.c_str());
				//printf("-------");

				if (Header.find("Transfer-Encoding: chunked") != std::string::npos) {
					IsChunked = true;
				}
				AlreadyAddedNewMsg = true;
			}
			if (GotHeaders) {
				if (!AlreadyAddedNewMsg) {
					CompleteReceive += ReceiveMsg;
					Body += ReceiveMsg;
				}
				//printf("%s", ReceiveMsg.c_str());
				//printf("%s", Body.c_str());
				if (IsChunked) {
                    //printf("CHUNKY!!!!!!!!!!!!!!");
					while (WorkOnBody(Body, ChunkedBody, CurrentChunkSize)) { // work on body until i receive false
                        //printf("lop");
					}

					if (CurrentChunkSize == -1) {
                        //printf("k bye");
                        //printf("Finished Request 1 %s", UrlPath.c_str());
						return ChunkedBody;
					}
				} else {
                    //printf("NOT CHUNKY???????????????");
                    int ContentLengthStart = Header.find("Content-Length:");
                    int ContentLengthEnd = Header.find("\r\n", ContentLengthStart + 16);
                    ContentLength = stoi(Header.substr(ContentLengthStart + 16, ContentLengthEnd - ContentLengthStart));
                    //printf("ContentLength: %d", ContentLength);
                    //printf("Body size: %d", Body.size());
                    if (Body.size() == ContentLength) {
                        //printf("Finished Request 2 %s", UrlPath.c_str());
                        return Body;
                    }
                }
			}
			AlreadyAddedNewMsg = false;
			ReceiveMsg.clear();
		}
	}
    //printf("Finished Request 3 %s", UrlPath.c_str());
	return "";
}

bool API::WorkOnBody(std::string& Body, std::string& ChunkedBody, int& CurrentChunkSize) {
	if (CurrentChunkSize == 0) { // no chunksize yet, check for one
		auto EndOfLine = Body.find_first_of("\r\n");
		if (EndOfLine == std::string::npos) { // failed to get a chunksize, that should not really be possible
			//std::cout << "Failed to get Chunksize" << std::endl;
			return false; 
		}
		//Log(Log::green, "%s", Body.c_str());

		auto ChunkSize = Body.substr(0, EndOfLine);
		Body = Body.substr(EndOfLine + 2);
		CurrentChunkSize = std::stol(ChunkSize, nullptr, 16);
		if (CurrentChunkSize == 0) { // return since i got the last chunk
			//Log(Log::red, "got last chunk");
			CurrentChunkSize = -1;
			return false;
		}
		else {
			return true;
		}
	}

	if (Body.size() >= CurrentChunkSize + 2) { // body size is big enouth for chunkedbody
		ChunkedBody += Body.substr(0, CurrentChunkSize);
		//Log(Log::blue, "%s", ChunkedBody.c_str());

		Body = Body.substr(CurrentChunkSize + 2);
		CurrentChunkSize = 0;
		return true;
	}

	return false;
}

bool API::DownloadFile(std::string Host, std::string UrlPath, std::string FilePath) {
    std::wstring HostName(utf8_decode(Host));
    int Port = 443;

    CEventWrapper ShutDownEvent;
    auto pActiveSock = std::make_unique<CActiveSock>(ShutDownEvent);
    pActiveSock->SetRecvTimeoutSeconds(30);
    pActiveSock->SetSendTimeoutSeconds(60);

    //printf("Connecting to %s %d", Host.c_str(), Port);
    bool b = pActiveSock->Connect(HostName.c_str(), static_cast<USHORT>(Port));
    if (!b) {
        //printf("Could not connect to the Server");
        return false;
    }
    //printf("Connected");

    auto pSSLClient = std::make_unique<CSSLClient>(pActiveSock.get());
    pSSLClient->ServerCertAcceptable = CertAcceptable;
    pSSLClient->SelectClientCertificate = SelectClientCertificate;
    HRESULT hr = pSSLClient->Initialize(HostName.c_str());
    if (!SUCCEEDED(hr)) {
        printf("Failed to Init SSL %llX", hr);
        return false;
    }

    std::string RequestString = "GET " + UrlPath + " HTTP/1.1\r\nAccept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.9\r\nAccept-Encoding: gzip, deflate, br\r\nAccept-Language: de-DE,de;q=0.9,en-US;q=0.8,en;q=0.7\r\nCache-Control: no-cache\r\nHost: " + Host + "\r\n\r\n";

    pSSLClient->Send(RequestString.c_str(), RequestString.size());

    //printf("Sent Request GET  %s", UrlPath.c_str());

    int BufferBytesReceiving = 4096;
    int ContentLength = 0;
    bool GotHeaders = false;
    bool RequestFinished = false;
    bool IsChunked = false;
    bool AlreadyAddedNewMsg = false;
    int CurrentChunkSize = 0;
    std::string CompleteReceive = "";
    std::string Header = "";
    std::string Body = "";
    std::string ChunkedBody = "";

    while (!RequestFinished) {
        std::string ReceiveMsgBuffer;
        ReceiveMsgBuffer.resize(BufferBytesReceiving);
        ReceiveMsgBuffer.reserve(BufferBytesReceiving);
        int BytesReceived = 0;

        int res = 1;
        res = (BytesReceived = pSSLClient->Recv(&ReceiveMsgBuffer[0], BufferBytesReceiving));
        if (0 < res)
        {
            std::string ReceiveMsg = ReceiveMsgBuffer.substr(0, BytesReceived);
            CompleteReceive += ReceiveMsg;
            if (!GotHeaders && CompleteReceive.find("\r\n\r\n") != std::string::npos) {
                Header = CompleteReceive.substr(0, CompleteReceive.find("\r\n\r\n"));
                Body = CompleteReceive.substr(CompleteReceive.find("\r\n\r\n") + 4);
                GotHeaders = true;

                if (Header.find("Transfer-Encoding: chunked") != std::string::npos) {
                    IsChunked = true;
                }
                AlreadyAddedNewMsg = true;
                //printf("GotHeaders: %s", Header.c_str());
            }
            if (GotHeaders) {
                if (!AlreadyAddedNewMsg) {
                    CompleteReceive += ReceiveMsg;
                    Body += ReceiveMsg;
                }
                if (IsChunked) {
                    while (WorkOnBody(Body, ChunkedBody, CurrentChunkSize)) { // work on body until i receive false
                        //printf("Working: %s", Header.c_str());
                    }

                    if (CurrentChunkSize == -1) {
                        RequestFinished = true;
                        Body = ChunkedBody;
                    }
                } else {
                    //printf("GotHeaders: %s", Header.c_str());
                    int ContentLengthStart = Header.find("Content-Length:");
                    int ContentLengthEnd = Header.find("\r\n", ContentLengthStart + 16);
                    ContentLength = std::stoi(Header.substr(ContentLengthStart + 16, ContentLengthEnd - ContentLengthStart - 16));
                    if (Body.size() >= ContentLength) {
                        RequestFinished = true;
                    }
                }
            }
            //printf("Received: %s", ReceiveMsg.c_str());
            AlreadyAddedNewMsg = false;
            ReceiveMsg.clear();
        }
        else if (res == 0)
        {
            //printf("Connection closed by server GET %s", UrlPath.c_str());
            RequestFinished = true;
        }
        else
        {
            //printf("Error receiving data GET %s", UrlPath.c_str());
            RequestFinished = true;
        }
    }

    if (Body.size() < ContentLength) {
        //printf("Content-Length mismatch! %d vs %d", Body.size(), ContentLength);
        return false;
    }
    Body.resize(ContentLength);

    //printf("Writing to file");

    if (Body.empty()) {
        return false;
    } else {
        //printf("Writing %d bytes to %s", Body.size(), FilePath.c_str());
        auto p = std::filesystem::path(FilePath);
        if (p.has_extension() && p.extension() == ".exe") {
            try {
                if (std::filesystem::exists(FilePath))
                    std::filesystem::rename(FilePath, FilePath + ".old");
            } catch (const std::exception& e) {
                printf("Error: %s\n", e.what());
            }
        } else if (!std::filesystem::exists(p.parent_path())) {
            try {
                std::filesystem::create_directories(p.parent_path());
            } catch (const std::exception& e) {
                printf("Error: %s\n", e.what());
                return false;
            }
        }
        std::ofstream myfile;
        myfile.open(FilePath, std::ofstream::out | std::ofstream::trunc | std::ofstream::binary);
        myfile << Body;
        myfile.close();
    }

    //printf("Downloaded %d bytes", Body.size());

    return true;
}