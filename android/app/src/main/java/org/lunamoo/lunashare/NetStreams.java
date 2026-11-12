package org.lunamoo.lunashare;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

// Internet radio: URLs read as they come, for the native player (any thread). Redirects are followed by hand, so
// http <-> https hops work too (HttpURLConnection only follows those within one protocol).
// A read never blocks long: a thread of each connection reads the network into a queue, and read() waits a quarter
// of a second at most ("nothing yet"), so the native side can give up at once when a station is changed or stopped -
// a read stuck in a stalled connection held the player (and with it the app's screen) for many seconds.
final class NetStreams {
    static final int NOTHING_YET = -2;
    private static final byte[] END = new byte[0], ERROR = new byte[0];

    private static final class Conn {
        HttpURLConnection c;
        InputStream in;
        final LinkedBlockingQueue<byte[]> q = new LinkedBlockingQueue<>(64);  // (up to 1 MB ahead)
        byte[] cur;
        int off;
        volatile boolean closed;

        void pump() {
            byte[] tail = ERROR;
            try {
                for (;;) {
                    byte[] b = new byte[16384];
                    int n = in.read(b, 0, b.length);
                    if (n < 0) {
                        tail = END;
                        break;
                    }
                    if (n == 0) continue;
                    byte[] d = n == b.length ? b : java.util.Arrays.copyOf(b, n);
                    while (!closed && !q.offer(d, 250, TimeUnit.MILLISECONDS)) {}
                    if (closed) return;
                }
            } catch (Throwable ignored) {
            }
            while (!closed) {
                try {
                    if (q.offer(tail, 250, TimeUnit.MILLISECONDS)) break;
                } catch (InterruptedException e) {
                    break;
                }
            }
        }
    }
    private static final ConcurrentHashMap<Integer, Conn> open = new ConcurrentHashMap<>();
    private static final AtomicInteger next = new AtomicInteger(1);

    // a connection being made (on a thread of its own: connecting and the server's answer can take many seconds)
    private static final class Opening {
        final java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
        volatile String[] result;
        volatile boolean aborted;
        volatile HttpURLConnection c;  // (the one being tried: let go of on an abort)
    }
    private static final ConcurrentHashMap<Integer, Opening> opening = new ConcurrentHashMap<>();

    // {id ("0" on failure, "-token" while still connecting: openWait(token)), status, headers ("name: value" lines),
    // final URL, error}
    static String[] open(String url, boolean icy) {
        final Opening o = new Opening();
        final int token = next.getAndIncrement();
        opening.put(token, o);
        Thread t = new Thread(() -> {
            String[] r = connect(url, icy, o);
            synchronized (o) {
                if (o.aborted) {  // (nobody waits for it any more)
                    int id = Integer.parseInt(r[0]);
                    if (id > 0) close(id);
                }
                o.result = r;
                o.done.countDown();
            }
        }, "LunaShare net open");
        t.setDaemon(true);
        t.start();
        return openWait(token);
    }

    static String[] openWait(int token) {
        Opening o = opening.get(token);
        if (o == null) return new String[] {"0", "0", "", "", "cancelled"};
        try {
            if (!o.done.await(250, TimeUnit.MILLISECONDS)) return new String[] {String.valueOf(-token), "0", "", "", ""};
        } catch (InterruptedException e) {
            return new String[] {String.valueOf(-token), "0", "", "", ""};
        }
        opening.remove(token);
        return o.result;
    }

    static void openAbort(int token) {
        final Opening o = opening.remove(token);
        if (o == null) return;
        synchronized (o) {
            o.aborted = true;
            if (o.done.getCount() == 0) {  // (it was just made: let go of)
                String[] r = o.result;
                int id = r == null ? 0 : Integer.parseInt(r[0]);
                if (id > 0) close(id);
                return;
            }
        }
        final HttpURLConnection c = o.c;
        if (c != null) {
            Thread t = new Thread(() -> { try { c.disconnect(); } catch (Throwable ignored) {} }, "LunaShare net abort");
            t.setDaemon(true);
            t.start();
        }
    }

    private static String[] connect(String url, boolean icy, Opening o) {
        String at = url;
        int status = 0;
        try {
            for (int hop = 0; hop < 6; hop++) {
                if (o.aborted) return new String[] {"0", String.valueOf(status), "", at, "cancelled"};
                HttpURLConnection c = (HttpURLConnection) new URL(at).openConnection();
                o.c = c;
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
                final Conn k = new Conn();
                k.c = c;
                k.in = c.getInputStream();
                int id = next.getAndIncrement();
                open.put(id, k);
                Thread t = new Thread(k::pump, "LunaShare net " + id);
                t.setDaemon(true);
                t.start();
                return new String[] {String.valueOf(id), String.valueOf(status), h.toString(), at, ""};
            }
            return new String[] {"0", String.valueOf(status), "", at, "too many redirects"};
        } catch (Exception e) {
            return new String[] {"0", String.valueOf(status), "", at, String.valueOf(e.getMessage())};
        }
    }

    // bytes read; 0 at the end; -1 on an error; NOTHING_YET when none came within a quarter of a second
    static int read(int id, byte[] buf) {
        Conn k = open.get(id);
        if (k == null) return -1;
        try {
            if (k.cur == null) {
                byte[] d = k.q.poll(250, TimeUnit.MILLISECONDS);
                if (d == null) return NOTHING_YET;
                if (d == END) {
                    k.q.offer(END);  // (and so on, for any later read)
                    return 0;
                }
                if (d == ERROR) {
                    k.q.offer(ERROR);
                    return -1;
                }
                k.cur = d;
                k.off = 0;
            }
            int n = Math.min(buf.length, k.cur.length - k.off);
            System.arraycopy(k.cur, k.off, buf, 0, n);
            k.off += n;
            if (k.off >= k.cur.length) k.cur = null;
            return n;
        } catch (Exception e) {
            return -1;
        }
    }

    // (never waits for the network: the connection is let go of on a thread of its own)
    static void close(int id) {
        final Conn k = open.remove(id);
        if (k == null) return;
        k.closed = true;
        k.q.clear();
        Thread t = new Thread(() -> {
            try { k.c.disconnect(); } catch (Throwable ignored) {}
            try { k.in.close(); } catch (Throwable ignored) {}
        }, "LunaShare net close");
        t.setDaemon(true);
        t.start();
    }
}
