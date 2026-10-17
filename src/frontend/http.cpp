#include "http.h"

#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#endif

namespace plumsg {

#ifdef _WIN32

int httpGet(const std::string& url, std::vector<uint8_t>& body) {
    body.clear();
    HINTERNET net = InternetOpenW(L"plumsg", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!net) return 0;
    DWORD timeout = 15000;
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof timeout);
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof timeout);
    const std::wstring wide(url.begin(), url.end());  // percent-encoded, so plain ASCII
    HINTERNET request = InternetOpenUrlW(net, wide.c_str(), nullptr, 0,
                                         INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_RELOAD, 0);
    int status = 0;
    if (request) {
        DWORD code = 0, size = sizeof code;
        if (HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &size, nullptr))
            status = static_cast<int>(code);
        char buffer[16384];
        DWORD read = 0;
        while (status == 200 && InternetReadFile(request, buffer, sizeof buffer, &read) && read > 0)
            body.insert(body.end(), buffer, buffer + read);
        InternetCloseHandle(request);
    }
    InternetCloseHandle(net);
    return status;
}

#else

int httpGet(const std::string& url, std::vector<uint8_t>& body) {
    body.clear();
    // The URL is percent-encoded, so it has no quote that could end the shell string.
    if (url.find('\'') != std::string::npos) return 0;
    const std::string command = "curl -sSL --max-time 20 -w '\\n%{http_code}' '" + url + "' 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return 0;
    char buffer[16384];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) body.insert(body.end(), buffer, buffer + n);
    pclose(pipe);
    // The status code is on the last line, after the body.
    size_t line = body.size();
    while (line > 0 && body[line - 1] != '\n') --line;
    int status = 0;
    for (size_t i = line; i < body.size(); ++i)
        if (body[i] >= '0' && body[i] <= '9') status = status * 10 + (body[i] - '0');
    body.resize(line > 0 ? line - 1 : 0);
    if (status != 200) body.clear();
    return status;
}

#endif

}  // namespace plumsg
