/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32
#include "xenia/app/check_update.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"

#include <build/version.h>
#include <algorithm>
#include <string>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <WinInet.h>
#include <shellapi.h>

#pragma comment (lib, "Wininet.lib")

using std::string;
using std::wstring;


namespace xe {
namespace update {

  // convert to wstring
  wstring CharPToWstring(const char* _charP) {
    return wstring(_charP, _charP + strlen(_charP));
  }

  // send https request
  wstring SendHTTPSRequest_GET(const wstring& _server,
                   const wstring& _page,
                   const wstring& _params = L"") {

    char szData[4096];

    // initialize WinInet
    HINTERNET hInternet =
		::InternetOpen(
			TEXT("Checking for updates"),INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (hInternet != NULL) {
      // open HTTP session
      HINTERNET hConnect =
		  ::InternetConnect(
			  hInternet, _server.c_str(), INTERNET_DEFAULT_HTTPS_PORT,
			  NULL,NULL, INTERNET_SERVICE_HTTP, 0, 1);
      if (hConnect != NULL) {
        wstring request =
			_page + (_params.empty() ? L"" : (L"?" + _params));

        // open request
        HINTERNET hRequest =
			::HttpOpenRequest(hConnect, L"GET",	(LPCWSTR)request.c_str(), 
				NULL, NULL,	0, (INTERNET_FLAG_SECURE), 1);
        if (hRequest != NULL) {   
          // send request
          BOOL isSend = ::HttpSendRequest(hRequest, NULL, 0, NULL, 0);

          if (isSend) {
            for(;;) {
            // reading data
            DWORD dwByteRead;
            BOOL isRead =
				::InternetReadFile(hRequest, szData, sizeof(szData) - 1, &dwByteRead);

            // break cycle if error or end
            if (isRead == FALSE || dwByteRead == 0) break;

            // saving result
            szData[dwByteRead] = 0;
            }
          }

          // close request
        ::InternetCloseHandle(hRequest);
        }
        // close session
        ::InternetCloseHandle(hConnect);
      }
      // close WinInet
      ::InternetCloseHandle(hInternet);
    }

    wstring answer = CharPToWstring(szData);
    // should we clean this up?
	  //delete [] szData;
    return answer;
  }

  void parse_response(wstring response, string *date, string *url ) {
    
    char buf[4096];
    char * pch;

    sprintf(buf, xe::to_string(response).c_str());
    pch = strtok(buf,",");

    while (pch != NULL) {
      if (strstr(pch, "created_at") && !((date)->length())) {
        *date = pch;
      }
      if (strstr(pch, "browser_download_url") && !((url)->length())) {
        *url = pch;
        // should we clean these up?
		    //delete [] buf;
		    //delete(pch);
        break;
      }
      pch = strtok (NULL, ",");
    }
  }

  int OfferUpdateWindow(wstring link) {
    int msgboxID = MessageBox(
        NULL,
        (LPCWSTR)L"There is a newer version available. Would you like to download the latest update?",
        // (LPCWSTR)link.c_str(),
        (LPCWSTR)L"Get the latest Xenia-Canary from GitHub",
        MB_ICONINFORMATION | MB_YESNO | MB_SETFOREGROUND | MB_TASKMODAL
    );
    switch (msgboxID) {
    case IDYES:
      return 1;
    case IDNO:
      return 0;
    }
    return 0;
  }

  void Update::CheckUpdate() {

    char buffer [80];
    string b_date, b_datetime, date, url = "";
    b_date = __DATE__;
    b_datetime = b_date + " " + __TIME__;    
    
    // current build time
    std::tm t_b = {};
    std::istringstream ss(b_datetime.c_str());
    // ss.imbue(std::locale("en_US.utf-8"));
    ss >> std::get_time(&t_b, "%b %d %Y %H:%M:%S");
    if (ss.fail()) {
      XELOGI("Parse failed");
    } else {
      strftime(buffer, 80, "### current built date : %m-%d-%Y %I:%M%p", &t_b);
      XELOGI("%s", buffer);
    }
    // time at runtime
    std::time_t t_n;
    time(&t_n); 
    std::tm* now = std::localtime(&t_n);
    strftime(buffer, 80, "### time now : %m-%d-%Y %I:%M%p", now);
    XELOGI("%s", buffer);

    // get difference of build/run time in hours
    double diff = (difftime(mktime(now), mktime(&t_b)) / 3600.0);
    //XELOGI("%f",diff);
    if (diff > 24.5) {
      //request api response for latest release
      wstring answer = SendHTTPSRequest_GET(L"api.github.com",
		    L"/repos/xenia-canary/xenia-canary/releases/latest", L"");

      //parse response for just the sections we're after
      parse_response(answer, &date, &url );

      // do some sanitation on the substrings we got back 
      std::replace(date.begin(), date.end(), '"', ' ');
      url.erase(0, url.find(':') + 2); //24
      url.erase(url.find('"'));
      // XELOGI("### Link to latest build : %s", url.c_str());

      // time of latest build on github
      std::tm t_l = {};
      std::istringstream ss2(date.c_str());
      // ss.imbue(std::locale("en_US.utf-8"));
      ss2 >> std::get_time(&t_l, " created_at : %Y-%m-%dT%H:%M:%S");
      if (ss2.fail()) {
        XELOGI("Parse failed");
      } else {
        strftime(buffer, 80, "### latest build: %m-%d-%Y %I:%M%p", &t_l);
        XELOGI("%s", buffer);
      }
      //diff latest/current build time in hours
      diff = (difftime(mktime(&t_l),mktime(&t_b)) / 3600.0);
      //XELOGI("%f",diff);
      if (diff > 0) {
        if (OfferUpdateWindow(xe::to_wstring(url))) {
          wstring wurl = xe::to_wstring(url);
          ShellExecute(0, 0, wurl.c_str(), 0, 0 , SW_SHOW );
        }
      }
    }
  }
}  // namespace update
}  // namespace xe
#endif  // XE_PLATFORM_WIN32