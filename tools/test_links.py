#!/usr/bin/env python3
"""Browser test of "links for anyone": a real Chrome downloads files from a test device (lunashare_tests in its
LUNASHARE_TEST_SERVE_LINK mode) through the test server and on the "local network".

    python3 tools/test_links.py build/lunashare_tests [--https]
"""
import hashlib, os, subprocess, sys, tempfile, time
from playwright.sync_api import sync_playwright

exe = os.path.abspath(sys.argv[1])
https = "--https" in sys.argv
work = tempfile.mkdtemp(prefix="lslinks_")
src = os.path.join(work, "plik testowy.bin")
with open(src, "wb") as f:
    f.write(os.urandom(5 * 1024 * 1024 + 4321))
want = hashlib.sha256(open(src, "rb").read()).hexdigest()
env = dict(os.environ, LUNASHARE_TEST_SERVE_LINK=src)
if https:
    cert, key = os.path.join(work, "c.pem"), os.path.join(work, "k.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "1",
                    "-subj", "/CN=127.0.0.1"], check=True, capture_output=True)
    env.update(LINK_CERT=cert, LINK_KEY=key)
p = subprocess.Popen([exe], env=env, stdout=subprocess.PIPE, text=True)
urls, toks, lanport = {}, {}, 0
for line in p.stdout:
    w = line.split()
    if not w: continue
    if w[0] in ("plain", "pin", "ask"): urls[w[0]] = w[1]
    if w[0] == "tok": toks[w[1]] = w[2]
    if w[0] == "lanport": lanport = int(w[1])
    if w[0] == "ready": break
print("links:", urls)
fails = 0

def check(cond, what):
    global fails
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond: fails += 1

def download(page, click_sel="#go", timeout=60000):
    with page.expect_download(timeout=timeout) as d:
        page.click(click_sel)
    path = d.value.path()
    download.url = d.value.url
    return d.value.suggested_filename, hashlib.sha256(open(path, "rb").read()).hexdigest()

try:
    with sync_playwright() as pw:
        b = pw.chromium.launch(executable_path="/usr/bin/google-chrome", args=["--no-sandbox"])
        ctx = b.new_context(accept_downloads=True, ignore_https_errors=True, locale="pl-PL", bypass_csp=True)
        page = ctx.new_page()
        page.on("console", lambda m: print("    console:", m.text) if m.type == "error" else None)

        # through the server, end to end encrypted
        page.goto(urls["plain"])
        page.wait_for_selector("#go:not([disabled])", timeout=20000)
        check(page.text_content("#name") == "plik testowy.bin", "the page shows the name (decrypted): " + page.text_content("#name"))
        check(want in page.text_content("#sha"), "and the SHA-256")
        name, h = download(page)
        check(name == "plik testowy.bin" and h == want, "downloaded and the same: %s" % name)
        page.wait_for_function("document.getElementById('msg').textContent.indexOf('✓')>=0", timeout=20000)
        check("sprawdzono" in page.text_content("#msg"), "says it was checked: " + page.text_content("#msg"))

        # a part changed on the way is caught and fetched again
        hits = {"n": 0}
        def tamper(route):
            r = route.fetch()
            body = bytearray(r.body())
            if hits["n"] == 0 and len(body) > 100:
                body[50] ^= 0xFF
            hits["n"] += 1
            route.fulfill(response=r, body=bytes(body))
        page2 = ctx.new_page()
        page2.route("**/api/*/p/1?*", tamper)
        page2.goto(urls["plain"])
        page2.wait_for_selector("#go:not([disabled])", timeout=20000)
        name, h = download(page2)
        check(h == want and hits["n"] >= 2, "a tampered part was refused and fetched again (%d tries)" % hits["n"])
        page2.close()

        # big files go to disk through the service worker (here: anything over 1 MB)
        page3 = ctx.new_page()
        u = urls["plain"].replace("#", "?mem=1#")
        page3.goto(u)
        page3.wait_for_selector("#go:not([disabled])", timeout=20000)
        name, h = download(page3, timeout=90000)
        check(h == want and ("/s/dl/" in download.url or https),  # (Chrome allows no service worker with a self-signed certificate)
              "streamed through the service worker: %s (%s)" % (name, download.url))
        page3.close()

        # PIN
        page4 = ctx.new_page()
        page4.goto(urls["pin"])
        page4.wait_for_selector("#pin:not(.hide)", timeout=20000)
        page4.fill("#pin", "1111")
        page4.click("#go")
        page4.wait_for_function("document.getElementById('msg').textContent.indexOf('4')>=0", timeout=20000)
        check("PIN" in page4.text_content("#msg"), "a wrong PIN: " + page4.text_content("#msg"))
        page4.fill("#pin", "2468")
        page4.click("#go")
        page4.wait_for_selector("#go:not([disabled])", timeout=20000)
        page4.wait_for_function("document.getElementById('pin').classList.contains('hide')", timeout=20000)
        name, h = download(page4)
        check(h == want, "the right PIN, then the file")
        page4.close()

        # ask me first (the test device says yes)
        page5 = ctx.new_page()
        page5.goto(urls["ask"])
        page5.wait_for_selector("#go:not([disabled])", timeout=30000)
        name, h = download(page5)
        check(h == want, "asked first, allowed, downloaded")
        page5.close()

        # on this network: straight from the device
        plain_id = urls["plain"].split("/s/")[1].split("#")[0]
        page6 = ctx.new_page()
        page6.goto("http://127.0.0.1:%d/f/%s" % (lanport, toks[plain_id]))
        page6.wait_for_selector("#go:not([disabled])", timeout=20000)
        name, h = download(page6)
        check(name == "plik testowy.bin" and h == want, "on this network: %s" % name)
        page6.close()

        # a broken link
        page7 = ctx.new_page()
        page7.goto(urls["plain"].split("#")[0] + "#abc")
        page7.wait_for_function("document.getElementById('msg').textContent.length>0", timeout=20000)
        check("niepełny" in page7.text_content("#msg"), "an incomplete link is explained: " + page7.text_content("#msg"))
        page7.close()
        page.screenshot(path=os.path.join(work, "page.png"))
        print("screenshot:", os.path.join(work, "page.png"))
        b.close()
finally:
    open(os.path.join(work, "stop"), "w").close()
    p.wait(timeout=20)
print("FAILURES: %d" % fails if fails else "ALL OK")
sys.exit(1 if fails else 0)
