
#include "os_support.h"
#if defined(_WIN32)
#include <cassert>
#include <functional>
#include <memory>
#include <vector>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "WinHttp.lib")
#pragma warning(disable: 4267)

std::wstring a2u(const std::string& s)
{
	if (s.empty())
	{
		return {};
	}
	int len = MultiByteToWideChar(CP_ACP, 0, s.data(), s.size(), nullptr, 0);
	if (!len)
	{
		return {};
	}
	std::wstring r;
	r.resize(len);
	MultiByteToWideChar(CP_ACP, 0, s.data(), s.size(), (LPWSTR)r.data(), r.size());
	return std::move(r);
}

std::string u2a(const std::wstring& s)
{
	if (s.empty())
	{
		return {};
	}
	int len = WideCharToMultiByte(CP_ACP, 0, s.data(), s.size(), nullptr, 0, nullptr, nullptr);
	if (len)
	{
		std::string r;
		r.resize(len);
		if (WideCharToMultiByte(CP_ACP, 0, s.data(), s.size(), (char*)r.data(), r.size(), nullptr, nullptr))
		{
			return std::move(r);
		}
	}
	return std::string();
}

template<typename T>
std::vector<T> splitString(const T& str, const T& delim)
{
	if (str.empty())
	{
		return {};
	}
	std::vector<T> result;
	size_t last = 0;
	size_t index = str.find_first_of(delim, last);
	while (index != T::npos)
	{
		T tt = str.substr(last, index - last);
		result.push_back(tt);
		last = index + delim.size();
		index = str.find_first_of(delim, last);
	}
	if (index - last > 0)
	{
		result.push_back(str.substr(last, index - last));
	}
	return result;
}

struct StructedURL
{
public:
	uint16_t port = 0;
	std::wstring scheme, host, path, extra;

	StructedURL() = default;
	~StructedURL() = default;
};

StructedURL ParseUrl(const std::wstring& url, bool escape)
{
	URL_COMPONENTSW urlComp = { 0 };
	urlComp.dwStructSize = sizeof(urlComp);

	// Set required component lengths to non-zero 
	// so that they are cracked.
	urlComp.dwSchemeLength = (DWORD)-1;
	urlComp.dwHostNameLength = (DWORD)-1;
	urlComp.dwUrlPathLength = (DWORD)-1;
	urlComp.dwExtraInfoLength = (DWORD)-1;

	auto done = !!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), /*escape ? ICU_ESCAPE : */0, &urlComp);
	if (done)
	{
		StructedURL result;
		result.port = urlComp.nPort;
		if (urlComp.lpszScheme != nullptr &&
			urlComp.dwSchemeLength > 0)
		{
			result.scheme = std::wstring(urlComp.lpszScheme, urlComp.dwSchemeLength);
		}
		if (urlComp.lpszHostName != nullptr &&
			urlComp.dwHostNameLength > 0)
		{
			result.host = std::wstring(urlComp.lpszHostName, urlComp.dwHostNameLength);
		}
		if (urlComp.lpszUrlPath != nullptr &&
			urlComp.dwUrlPathLength > 0)
		{
			result.path = std::wstring(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
		}
		if (urlComp.lpszExtraInfo != nullptr &&
			urlComp.dwExtraInfoLength > 0)
		{
			result.extra = std::wstring(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength);
		}
		return result;
	}
	return {};
}

int PerformHttp(
	const std::string& method,
	const std::string& url,
	bool autoProxy,
	const HttpHeaders& headers,
	const std::string& body,
	HttpHeaders& responseHeaders,
	HttpContentHandler&& contentHandler)
{
	auto unicodeUrl = a2u(url);
	if (unicodeUrl.empty())
	{
		return -1;
	}

	auto comps = ParseUrl(unicodeUrl, true);
	if (!comps.port)
	{
		return -1;
	}
	auto secure = _wcsicmp(comps.scheme.c_str(), L"https") == 0;

	// Use WinHttpOpen to obtain a session handle.
	auto uaIt = headers.find("User-Agent");
	auto ua = (uaIt == headers.end()) ? L"WinHttp" : a2u(uaIt->second);
	auto hSession = WinHttpOpen(
		ua.c_str(),
		WINHTTP_ACCESS_TYPE_NO_PROXY,
		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS,
		0);

	// Prepare proxy
	if (autoProxy)
	{
		WINHTTP_CURRENT_USER_IE_PROXY_CONFIG IEProxyConfig = { 0 };
		if (WinHttpGetIEProxyConfigForCurrentUser(&IEProxyConfig))
		{
			DWORD						dwProxyAuthScheme = 0;
			WINHTTP_AUTOPROXY_OPTIONS	AutoProxyOptions = { 0 };
			WINHTTP_PROXY_INFO			ProxyInfo = { 0 };
			DWORD						cbProxyInfoSize = sizeof(ProxyInfo);

			if (IEProxyConfig.fAutoDetect)
			{
				AutoProxyOptions.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;

				//
				// Use both DHCP and DNS-based auto detection
				//
				AutoProxyOptions.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP |
					WINHTTP_AUTO_DETECT_TYPE_DNS_A;
			}

			//
			// If there's an auto config URL stored in the IE proxy settings, save it
			//
			if (IEProxyConfig.lpszAutoConfigUrl)
			{
				AutoProxyOptions.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
				AutoProxyOptions.lpszAutoConfigUrl = IEProxyConfig.lpszAutoConfigUrl;
			}

			//
			// If there's a static proxy
			//
			if (IEProxyConfig.lpszProxy)
			{
				AutoProxyOptions.dwFlags |= WINHTTP_AUTOPROXY_ALLOW_STATIC;
			}

			// If obtaining the PAC script requires NTLM/Negotiate
			// authentication, then automatically supply the client
			// domain credentials.
			AutoProxyOptions.fAutoLogonIfChallenged = TRUE;

			//
			// Call WinHttpGetProxyForUrl with our target URL. If 
			// auto-proxy succeeds, then set the proxy info on the 
			// request handle. If auto-proxy fails, ignore the error 
			// and attempt to send the HTTP request directly to the 
			// target server (using the default WINHTTP_ACCESS_TYPE_NO_PROXY 
			// configuration, which the request handle will inherit 
			// from the session).
			//
			if (WinHttpGetProxyForUrl(hSession,
				unicodeUrl.c_str(),
				&AutoProxyOptions,
				&ProxyInfo))
			{
				// A proxy configuration was found, set it on the
				// request handle.
				if (!WinHttpSetOption(hSession,
					WINHTTP_OPTION_PROXY,
					&ProxyInfo,
					cbProxyInfoSize))
				{
					// Exit if setting the proxy info failed.
					WinHttpCloseHandle(hSession);
					return -1;
				}
			}
		}
	}

	// Connect
	auto hConnect = WinHttpConnect(hSession, comps.host.c_str(), comps.port, 0);
	if (!hConnect)
	{
		WinHttpCloseHandle(hSession);
		return -1;
	}

	// Prepare request
	DWORD flag = secure ? WINHTTP_FLAG_SECURE : 0;
	auto queryStr = comps.path + comps.extra;
	auto hRequest = WinHttpOpenRequest(
		hConnect,
		a2u(method).c_str(),
		queryStr.c_str(),
		nullptr,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		WINHTTP_FLAG_REFRESH | flag);
	if (!hRequest)
	{
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return -1;
	}

	// Concatenate headers
	std::wstring plainRequestHeaders;
	if (!headers.empty())
	{
		for (auto [k, v] : headers)
		{
			if (_stricmp(k.c_str(), "User-Agent") == 0)
			{
				continue;
			}
			plainRequestHeaders += a2u(k);
			plainRequestHeaders += L": ";
			plainRequestHeaders += a2u(v);
			plainRequestHeaders += L"\r\n";
		}
	}

#define FULL_CLOSE()	WinHttpCloseHandle(hRequest); \
						WinHttpCloseHandle(hConnect); \
						WinHttpCloseHandle(hSession)

	// Send requests
	bool done = false;
	do
	{
		if (plainRequestHeaders.empty())
		{
			done = !!WinHttpSendRequest(hRequest,
				WINHTTP_NO_ADDITIONAL_HEADERS, 0,
				(LPVOID)body.data(), body.size(),
				body.size(), 0);
		}
		else
		{
			done = WinHttpSendRequest(hRequest,
				plainRequestHeaders.c_str(), plainRequestHeaders.size(),
				(LPVOID)body.data(), body.size(),
				body.size(), 0);
		}
	} while (!done && GetLastError() == ERROR_WINHTTP_RESEND_REQUEST);
	if (!done)
	{
		FULL_CLOSE();
		return -1;
	}

	// Receive response
	if (!WinHttpReceiveResponse(hRequest, nullptr))
	{
		FULL_CLOSE();
		return -1;
	}

	// Context
	DWORD statusCode = 0;
	auto dwSize = (DWORD)sizeof(statusCode);
	done = WinHttpQueryHeaders(
		hRequest,
		WINHTTP_QUERY_STATUS_CODE |
		WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX,
		&statusCode,
		&dwSize,
		WINHTTP_NO_HEADER_INDEX);
	if (!done)
	{
		FULL_CLOSE();
		return -1;
	}

	// Read & parse headers
	dwSize = 0;
	WinHttpQueryHeaders(
		hRequest,
		WINHTTP_QUERY_RAW_HEADERS_CRLF,
		WINHTTP_HEADER_NAME_BY_INDEX,
		WINHTTP_NO_OUTPUT_BUFFER,
		&dwSize,
		WINHTTP_NO_HEADER_INDEX);
	if (!dwSize || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
	{
		FULL_CLOSE();
		return -1;
	}

	std::wstring plainRespHeaders;
	plainRespHeaders.resize(dwSize / sizeof(wchar_t));
	if (plainRespHeaders.empty())
	{
		FULL_CLOSE();
		return -1;
	}

	done = WinHttpQueryHeaders(
		hRequest,
		WINHTTP_QUERY_RAW_HEADERS_CRLF,
		WINHTTP_HEADER_NAME_BY_INDEX,
		(LPVOID)&plainRespHeaders[0], &dwSize,
		WINHTTP_NO_HEADER_INDEX);
	if (!done)
	{
		FULL_CLOSE();
		return -1;
	}

	size_t contentLength = 0;
	HttpHeaders localRespHeaders;
	auto lines = splitString<std::wstring>(plainRespHeaders, L"\r\n");
	for (const auto& line : lines)
	{
		if (line.empty())
		{
			continue;
		}
		auto pos = line.find_first_of(L':');
		if (pos > 0 && pos < line.size() - 2)
		{
			auto key = u2a(line.substr(0, pos));
			auto value = u2a(line.substr(pos + 2));
			if (!key.empty() && !value.empty())
			{
				localRespHeaders[key] = value;
			}
		}
	}

	// Check body length
	auto contentLenIt = localRespHeaders.find("Content-Length");
	if (contentLenIt != localRespHeaders.end())
	{
		contentLength = std::stol(contentLenIt->second);
	}
	if (!contentLength)
	{
		auto transEncIt = localRespHeaders.find("Transfer-Encoding");
		if (transEncIt == localRespHeaders.end() ||
			_stricmp(transEncIt->second.c_str(), "chunked") != 0)
		{
			// no content
			FULL_CLOSE();
			responseHeaders = std::move(responseHeaders);
			return statusCode;
		}
	}

	// Read in loop
	std::string buf;
	buf.resize(2 * 1024);
	if (buf.empty())
	{
		FULL_CLOSE();
		return -1;
	}

	size_t read = 0;
	while (true)
	{
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(hRequest, &avail))
		{
			// detect fail
			FULL_CLOSE();
			return -1;
		}
		if (!avail)
		{
			// no more data
			break;
		}

		auto bytesToRead = min(avail, buf.size());
		if (contentLength)
		{
			assert(read < contentLength);
			bytesToRead = min(bytesToRead, contentLength - read);
		}

		DWORD out = 0;
		if (!WinHttpReadData(hRequest, (void*)&buf[0], bytesToRead, &out))
		{
			// read fail
			FULL_CLOSE();
			return -1;
		}

		if (!contentHandler(contentLength, (const void*)&buf[0], out))
		{
			// user cancel
			FULL_CLOSE();
			return -1;
		}

		read += out;
		if (contentLength && read >= contentLength)
		{
			// completed
			break;
		}
	}
	FULL_CLOSE();

	if (contentLength && read < contentLength)
	{
		// incomplete
		return -1;
	}

	// done
	responseHeaders = std::move(localRespHeaders);
	return statusCode;
}

int http_get(const std::string& url, const HttpHeaders& requestHeaders, HttpContentHandler&& handler)
{
	HttpHeaders respHeaders;
	return PerformHttp("GET", url, true, requestHeaders, "", respHeaders, std::forward<HttpContentHandler>(handler));
}

bool is_acceptable_os_version(const std::string& osMinRequiredVersion)
{
	if (osMinRequiredVersion.empty())
	{
		return true;
	}

	OSVERSIONINFOEXW osvi = { sizeof(osvi), 0, 0, 0, 0, { 0 }, 0, 0 };
	DWORDLONG const dwlConditionMask = VerSetConditionMask(
		VerSetConditionMask(
			VerSetConditionMask(
				0, VER_MAJORVERSION, VER_GREATER_EQUAL),
			VER_MINORVERSION, VER_GREATER_EQUAL),
		VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);

	sscanf_s(osMinRequiredVersion.c_str(), "%lu.%lu.%hu", &osvi.dwMajorVersion, &osvi.dwMinorVersion, &osvi.wServicePackMajor);
	return !VerifyVersionInfoW(&osvi, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR, dwlConditionMask);
}

bool is_matched_os_name(const std::string& osName)
{
	if (_stricmp(osName.c_str(), "windows") == 0) return true;
#ifdef _WIN64
	if (_stricmp(osName.c_str(), "windows-x64") != 0) return true;
#else
	if (_stricmp(osName.c_str(), "windows-x86") != 0) return true;
#endif
	return false;
}

bool execute(const std::string& package, const std::string& args)
{
	SHELLEXECUTEINFOA sei = { 0 };
	sei.cbSize = sizeof(sei);
	sei.lpFile = package.c_str();
	sei.nShow = SW_SHOWDEFAULT;
	sei.fMask = SEE_MASK_FLAG_NO_UI;	// We display our own dialog box on error

	if (!args.empty())
	{
		sei.lpParameters = args.c_str();
	}

	return !!ShellExecuteExA(&sei);
}

std::string get_iso639_user_lang()
{
	std::string lang;
	lang.resize(2);
	auto langid = GetUserDefaultLangID();
	GetLocaleInfoA(langid, LOCALE_SISO639LANGNAME, &lang[0], lang.size());
	return std::move(lang);
}

#ifdef _USRDLL
BOOL WINAPI DllMain(
	HINSTANCE hinstDLL,  // handle to DLL module
	DWORD fdwReason,     // reason for calling function
	LPVOID lpReserved)  // reserved
{
	// Perform actions based on the reason for calling.
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		// Initialize once for each new process.
		// Return FALSE to fail DLL load.
		break;

	case DLL_THREAD_ATTACH:
		// Do thread-specific initialization.
		break;

	case DLL_THREAD_DETACH:
		// Do thread-specific cleanup.
		break;

	case DLL_PROCESS_DETACH:
		// Perform any necessary cleanup.
		break;
	}
	return TRUE;  // Successful DLL_PROCESS_ATTACH.
}
#endif //_USRDLL

#endif
