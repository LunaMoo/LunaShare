package org.lunamoo.lunashare;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

// Internet radio: URLs read as they come, for the native player (any thread). Redirects are followed by hand, so
// http <-> https hops work too (HttpURLConnection only follows those within one protocol).
final class NetStreams {
    private static final class Conn {
        HttpURLConnection c;
        InputStream in;
    }
    private static final ConcurrentHashMap<Integer, Conn> open = new ConcurrentHashMap<>();
    private static final AtomicInteger next = new AtomicInteger(1);

    // {id ("0" on failure), status, headers ("name: value" lines), final URL, error}
    static String[] open(String url, boolean icy) {
        String at = url;
        int status = 0;
        try {
            for (int hop = 0; hop < 6; hop++) {
                HttpURLConnection c = (HttpURLConnection) new URL(at).openConnection();
                c.setInstanceFollowRedirects(false);
                c.setConnectTimeout(10000);
                c.setReadTimeout(15000);
                c.setRequestProperty("User-Agent", "LunaShare/1.6 (internet radio)");
                c.setRequestProperty("Accept", "*/*");
                if (icy) c.setRequestProperty("Icy-MetaData", "1");
                status = c.getResponseCode();
                if (status >= 300 && status < 400) {
                    String loc = c.getHeaderField("Location");
                    c.disconnect();
                    if (loc == null) break;
                    at = new URL(new URL(at), loc).toString();
                    continue;
                }
                StringBuilder h = new StringBuilder();
                for (Map.Entry<String, List<String>> e : c.getHeaderFields().entrySet()) {
                    if (e.getKey() == null || e.getValue() == null || e.getValue().isEmpty()) continue;
                    h.append(e.getKey()).append(": ").append(e.getValue().get(0)).append('\n');
                }
                if (status < 200 || status >= 300) {
                    c.disconnect();
                    return new String[] {"0", String.valueOf(status), h.toString(), at, "HTTP " + status};
                }
                Conn k = new Conn();
                k.c = c;
                k.in = c.getInputStream();
                int id = next.getAndIncrement();
                open.put(id, k);
                return new String[] {String.valueOf(id), String.valueOf(status), h.toString(), at, ""};
            }
            return new String[] {"0", String.valueOf(status), "", at, "too many redirects"};
        } catch (Exception e) {
            return new String[] {"0", String.valueOf(status), "", at, String.valueOf(e.getMessage())};
        }
    }

    static int read(int id, byte[] buf) {
        Conn k = open.get(id);
        if (k == null) return -1;
        try {
            int n = k.in.read(buf, 0, buf.length);
            return n < 0 ? 0 : n;
        } catch (Exception e) {
            return -1;
        }
    }

    static void close(int id) {
        Conn k = open.remove(id);
        if (k == null) return;
        try { k.in.close(); } catch (Exception ignored) {}
        k.c.disconnect();
    }
}
