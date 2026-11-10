// LunaShare - the page a "link for anyone" opens in a browser (share_links.h). One page for both ways:
//   {"mode":"lan", name, size, sha, pin}  served by the device itself: checks, then the browser downloads the file
//   {"mode":"web"}                        served by the rendezvous server: everything else comes encrypted from the
//                                         device; the page decrypts and checks every part (key after '#')
#pragma once
#include <string>

namespace ls {

std::string LinkPageHtml(const std::string& cfgJson);
// streams big downloads to disk instead of memory (served at /sw.js, scope /s/)
const char* LinkServiceWorkerJs();
// extra response headers for the page (CSP and friends), each ending in \r\n
extern const char* const kLinkPageHeaders;

}  // namespace ls
