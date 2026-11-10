/*
 * LunaShare torrent module interface (C ABI): downloads torrents (.torrent files and magnet links).
 *
 * Optional like the other modules: lunashare_torrent.dll / liblunashare_torrent.so is loaded when present (desktops).
 * The module owns the BitTorrent session (libtorrent) and keeps its torrents between runs in the state folder. Lists
 * and settings cross the boundary as JSON text (UTF-8), so the interface stays small.
 *
 * Threading: any thread may call; read() blocks (up to its timeout) and is meant for a reader thread.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef LUNASHARE_TORRENT_H
#define LUNASHARE_TORRENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LST_API_VERSION 1

#if defined(_WIN32)
#define LST_EXPORT __declspec(dllexport)
#else
#define LST_EXPORT __attribute__((visibility("default")))
#endif

typedef struct LstHost {
    int api_version;
    void* user;
    void (*log)(void* user, const char* message);
} LstHost;

/* settings_json: {"down": KiB/s (0 = no limit), "up": KiB/s, "port": listen port (0 = any),
 *                 "seed": -1 keep seeding | 0 stop when done | N > 0: until N% of the size was uploaded,
 *                 "dht": bool, "upnp": bool, "agent": "LunaShare/1.5.9",
 *                 "limitLocal": bool (init only: the limits also count for peers on the local network)}
 * options_json (add): {"dir": folder to save into, "paused": bool}
 * list() -> [{"id": 40 hex, "name", "dir", "state": "meta" | "checking" | "down" | "seed" | "done" | "paused" | "error",
 *             "error", "size" (wanted bytes), "done" (bytes), "downRate", "upRate" (B/s), "uploaded", "peers", "seeds",
 *             "files" (count), "added" (unix s), "paused": bool, "single": bool (one file, saved without a folder),
 *             "path": the file or folder on disk}]
 * files(id) -> [{"i", "path" (inside the torrent), "size", "done", "want": bool}]  */
typedef struct LstModule {
    int api_version;
    const char* name;
    const char* version; /* of the library inside */
    int (*init)(const LstHost* host, const char* state_dir_utf8, const char* settings_json);
    void (*shutdown)(void); /* saves what's needed to go on next time */
    void (*apply_settings)(const char* settings_json);
    /* source: a magnet link, or the path of a .torrent file. id_out: at least 41 bytes. Returns 1 when added (also
       when it was there already), 0 with err filled otherwise. */
    int (*add)(const char* source_utf8, const char* options_json, char* id_out, char* err, int err_len);
    /* the same from a .torrent file's bytes */
    int (*add_data)(const uint8_t* data, int size, const char* options_json, char* id_out, char* err, int err_len);
    const char* (*list)(void);           /* valid until the same thread calls list() again */
    const char* (*files)(const char* id); /* likewise */
    void (*set_wanted)(const char* id, const char* wanted_json); /* [true, false, ...] per file */
    void (*pause)(const char* id, int paused);
    void (*remove)(const char* id, int delete_files);
    /* watching while it downloads: this file first, in order (on = 0: back to normal) */
    void (*stream)(const char* id, int file, int on);
    /* bytes of a file of the torrent, waiting (up to timeout_ms) until they have arrived - what is asked for is
       fetched first. Returns the bytes read (maybe fewer than len), 0 at the end of the file or when nothing came
       in time (see *timed_out), < 0 when the torrent is gone. */
    int (*read)(const char* id, int file, int64_t offset, uint8_t* buf, int len, int timeout_ms, int* timed_out);
    /* connect to this peer too (tests, or a known source) */
    void (*add_peer)(const char* id, const char* ip, int port);
} LstModule;

typedef const LstModule* (*LstGetModuleFn)(void);
LST_EXPORT const LstModule* lunashare_torrent_module(void);

#ifdef __cplusplus
}
#endif
#endif
