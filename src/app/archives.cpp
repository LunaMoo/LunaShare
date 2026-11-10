#include "archives.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <clocale>
#include <map>
#include <mutex>
#include <set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "platform.h"

extern "C" {
#include <archive.h>
#include <archive_entry.h>
}

using namespace ls;

namespace arc {

static std::string Low(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// a file's time as unix seconds (the filesystem clock's own start differs between systems)
static time_t UnixMTime(const std::string& p) {
    namespace fs = std::filesystem;
    using namespace std::chrono;
    std::error_code ec;
    auto t = fs::last_write_time(fs::u8path(p), ec);
    if (ec) return 0;
    auto sys = time_point_cast<system_clock::duration>(t - fs::file_time_type::clock::now() + system_clock::now());
    return system_clock::to_time_t(sys);
}

// libarchive turns names between UTF-8 and "the locale's" characters: the locale must be a UTF-8 one (Linux / Android
// start in "C", plain ASCII). Windows works with wide characters instead.
static void Utf8Locale() {
#ifndef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        for (const char* l : {"C.UTF-8", "C.utf8", "en_US.UTF-8", "en_US.utf8"})
            if (setlocale(LC_CTYPE, l)) break;
    });
#endif
}

static bool EndsWith(const std::string& s, const char* e) {
    size_t n = strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}

bool IsComicPath(const std::string& path) {
    std::string l = Low(path);
    return EndsWith(l, ".cbz") || EndsWith(l, ".cbr") || EndsWith(l, ".cb7") || EndsWith(l, ".cbt");
}

bool IsArchivePath(const std::string& path) {
    static const char* exts[] = {".zip", ".7z", ".rar", ".tar", ".gz", ".tgz", ".bz2", ".tbz", ".tbz2", ".xz", ".txz", ".zst", ".tzst",
                                 ".lz", ".lzma", ".iso", ".cab", ".lzh", ".lha", ".cpio", ".ar", ".deb", ".rpm", ".apk", ".jar", ".xpi",
                                 ".cbz", ".cbr", ".cb7", ".cbt", ".001"};
    std::string l = Low(path);
    for (auto* e : exts)
        if (EndsWith(l, e)) return true;
    return false;
}

std::string SafeRelPath(const std::string& p) {
    std::string out, part;
    auto flush = [&] {
        // (no empty / . / .. parts; characters Windows and phone memory cards refuse become _)
        while (!part.empty() && (part.back() == ' ' || part.back() == '.') && part != "." && part != "..") part.pop_back();
        if (!part.empty() && part != "." && part != "..") {
            for (auto& c : part)
                if ((unsigned char)c < 32 || strchr("<>:\"|?*", c)) c = '_';
            if (!out.empty()) out += '/';
            out += part;
        }
        part.clear();
    };
    for (size_t i = 0; i < p.size(); i++) {
        char c = p[i];
        if (c == '/' || c == '\\') flush();
        else part += c;
    }
    flush();
    return out;
}

// ---- reading through our own file (UTF-8 paths on Windows, 64-bit seeking for 7z / zip)
namespace {
struct Src {
    FILE* f = nullptr;
    std::vector<char> buf = std::vector<char>(1 << 17);
};
int64_t Tell(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}
int Seek(FILE* f, int64_t off, int whence) {
#ifdef _WIN32
    return _fseeki64(f, off, whence);
#else
    return fseeko(f, (off_t)off, whence);
#endif
}
la_ssize_t RdRead(struct archive*, void* d, const void** b) {
    Src* s = (Src*)d;
    size_t n = fread(s->buf.data(), 1, s->buf.size(), s->f);
    *b = s->buf.data();
    return (la_ssize_t)n;
}
la_int64_t RdSeek(struct archive*, void* d, la_int64_t off, int whence) {
    Src* s = (Src*)d;
    if (Seek(s->f, off, whence) != 0) return ARCHIVE_FATAL;
    return Tell(s->f);
}
la_int64_t RdSkip(struct archive*, void* d, la_int64_t req) {
    Src* s = (Src*)d;
    int64_t at = Tell(s->f);
    if (Seek(s->f, 0, SEEK_END) != 0) return 0;
    int64_t end = Tell(s->f);
    int64_t to = std::min(end, at + req);
    Seek(s->f, to, SEEK_SET);
    return to - at;
}
int RdClose(struct archive*, void* d) {
    Src* s = (Src*)d;
    if (s->f) fclose(s->f);
    s->f = nullptr;
    return ARCHIVE_OK;
}

// an archive opened for reading (nullptr: err says why)
struct archive* OpenRead(const std::string& path, const std::string& password, Src& src, std::string& err) {
    Utf8Locale();
    src.f = OpenFile(path, "rb");
    if (!src.f) {
        err = "can't open the file";
        return nullptr;
    }
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    std::string l = Low(path);
    // a single packed file (.gz, .xz...): one entry
    if (!EndsWith(l, ".tar") && (EndsWith(l, ".gz") || EndsWith(l, ".bz2") || EndsWith(l, ".xz") || EndsWith(l, ".zst") || EndsWith(l, ".lz") ||
                                 EndsWith(l, ".lzma")))
        archive_read_support_format_raw(a);
    if (!password.empty()) archive_read_add_passphrase(a, password.c_str());
    archive_read_set_read_callback(a, RdRead);
    archive_read_set_seek_callback(a, RdSeek);
    archive_read_set_skip_callback(a, RdSkip);
    archive_read_set_close_callback(a, RdClose);
    archive_read_set_callback_data(a, &src);
    if (archive_read_open1(a) != ARCHIVE_OK) {
        err = archive_error_string(a) ? archive_error_string(a) : "not an archive LunaShare can read";
        archive_read_free(a);
        return nullptr;
    }
    return a;
}

std::string EntryName(struct archive_entry* e, const std::string& archivePath) {
    const char* n = archive_entry_pathname_utf8(e);
    if (!n) n = archive_entry_pathname(e);
    std::string s = n ? n : "";
    if (s == "data") {  // (raw: a single packed file - named after the archive, without .gz / .xz ...)
        std::string b = BaseName(archivePath);
        size_t dot = b.find_last_of('.');
        if (dot != std::string::npos && dot > 0) s = b.substr(0, dot);
    }
    return s;
}

bool PasswordProblem(const char* msg) {
    if (!msg) return false;
    std::string m = Low(msg);
    return m.find("passphrase") != std::string::npos || m.find("encrypt") != std::string::npos || m.find("password") != std::string::npos;
}
}  // namespace

bool List(const std::string& path, Listing& out, const std::string& password) {
    out = Listing();
    Src src;
    struct archive* a = OpenRead(path, password, src, out.error);
    if (!a) return false;
    std::set<std::string> have;
    struct archive_entry* e;
    int r;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        Entry en;
        en.path = SafeRelPath(EntryName(e, path));
        if (en.path.empty()) continue;
        en.dir = archive_entry_filetype(e) == AE_IFDIR;
        en.size = archive_entry_size_is_set(e) ? archive_entry_size(e) : 0;
        en.mtime = archive_entry_mtime_is_set(e) ? (int64_t)archive_entry_mtime(e) : 0;
        en.encrypted = archive_entry_is_encrypted(e) != 0;
        if (en.encrypted) out.encrypted = true;
        if (out.format.empty() && archive_format_name(a)) out.format = archive_format_name(a);
        if (have.insert(en.path).second) out.entries.push_back(en);
        else if (!en.dir)  // (the same name twice: the later one)
            for (auto& x : out.entries)
                if (x.path == en.path) x = en;
    }
    if (r == ARCHIVE_FATAL) {
        const char* m = archive_error_string(a);
        out.error = m ? m : "the archive is damaged";
        if (PasswordProblem(m)) out.needPassword = out.encrypted = true;
    }
    if (archive_read_has_encrypted_entries(a) > 0) out.encrypted = true;
    archive_read_free(a);
    // folders that only show in the paths
    std::vector<Entry> dirs;
    for (auto& en : out.entries) {
        size_t s = 0;
        while ((s = en.path.find('/', s)) != std::string::npos) {
            std::string d = en.path.substr(0, s);
            if (have.insert(d).second) {
                Entry de;
                de.path = d;
                de.dir = true;
                dirs.push_back(de);
            }
            s++;
        }
    }
    out.entries.insert(out.entries.end(), dirs.begin(), dirs.end());
    return out.error.empty() || !out.entries.empty();
}

bool Extract(const std::string& path, const std::string& dest, const std::vector<std::string>& only, const std::string& password,
             const Progress& progress, std::string& err, std::vector<std::string>* written, const std::string& strip) {
    err.clear();
    // how much: from a listing (the total for the progress bar)
    int64_t total = 0, done = 0;
    {
        Listing l;
        List(path, l, password);
        for (auto& en : l.entries) {
            if (en.dir) continue;
            bool want = only.empty();
            for (auto& o : only)
                if (en.path == o || en.path.compare(0, o.size() + 1, o + "/") == 0) want = true;
            if (want) total += en.size;
        }
    }
    Src src;
    struct archive* a = OpenRead(path, password, src, err);
    if (!a) return false;
    MakeDirs(dest);
    std::vector<char> buf;
    struct archive_entry* e;
    int r;
    bool ok = true;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        std::string rel = SafeRelPath(EntryName(e, path));
        if (rel.empty()) continue;
        bool want = only.empty();
        for (auto& o : only)
            if (rel == o || rel.compare(0, o.size() + 1, o + "/") == 0) want = true;
        if (!want) continue;
        if (!strip.empty()) {
            if (rel == strip) continue;
            if (rel.compare(0, strip.size() + 1, strip + "/") == 0) rel = rel.substr(strip.size() + 1);
        }
        std::string to = JoinPath(dest, rel);
        unsigned type = archive_entry_filetype(e);
        if (type == AE_IFDIR) {
            MakeDirs(to);
            continue;
        }
        if (type != AE_IFREG && type != 0) continue;  // (links, devices: never)
        if (archive_entry_hardlink(e)) continue;
        MakeDirs(DirName(to));
        FILE* f = OpenFile(to, "wb");
        if (!f) {
            err = "can't write " + to;
            ok = false;
            break;
        }
        if (progress && !progress(done, total, rel)) {
            fclose(f);
            RemoveFile(to);
            err = "stopped";
            ok = false;
            break;
        }
        const void* blk;
        size_t sz;
        la_int64_t off, pos = 0;
        int rr;
        while ((rr = archive_read_data_block(a, &blk, &sz, &off)) == ARCHIVE_OK || rr == ARCHIVE_WARN) {
            if (off > pos) Seek(f, off, SEEK_SET);  // (sparse files: a hole)
            if (sz && fwrite(blk, 1, sz, f) != sz) {
                rr = ARCHIVE_FATAL;
                break;
            }
            pos = off + (la_int64_t)sz;
            done += (int64_t)sz;
            if (progress && !progress(done, total, rel)) {
                rr = ARCHIVE_FATAL;
                err = "stopped";
                break;
            }
        }
        fclose(f);
        if (rr != ARCHIVE_EOF) {
            RemoveFile(to);
            if (err.empty()) {
                const char* m = archive_error_string(a);
                err = m ? m : "the archive is damaged";
            }
            ok = false;
            break;
        }
        if (written) written->push_back(to);
    }
    if (ok && r == ARCHIVE_FATAL) {
        const char* m = archive_error_string(a);
        err = m ? m : "the archive is damaged";
        ok = false;
    }
    archive_read_free(a);
    return ok;
}

const char* FormatExt(Format f) {
    switch (f) {
        case Format::Zip: return ".zip";
        case Format::SevenZip: return ".7z";
        case Format::TarGz: return ".tar.gz";
        case Format::TarXz: return ".tar.xz";
        case Format::TarZst: return ".tar.zst";
    }
    return ".zip";
}

namespace {
la_ssize_t WrWrite(struct archive*, void* d, const void* b, size_t n) {
    return (la_ssize_t)fwrite(b, 1, n, (FILE*)d);
}
int WrClose(struct archive*, void* d) {
    return fclose((FILE*)d) == 0 ? ARCHIVE_OK : ARCHIVE_FATAL;
}
}  // namespace

bool Create(const std::string& out, Format f, const std::vector<std::string>& inputs, const Progress& progress, std::string& err) {
    err.clear();
    // what goes in: (the file, its name inside)
    std::vector<std::pair<std::string, std::string>> files;
    std::set<std::string> names;
    for (auto& in : inputs) {
        std::vector<std::pair<std::string, std::string>> got;
        if (IsDirectory(in)) CollectFiles(in, got);
        else got.push_back({in, BaseName(in)});
        for (auto& [abs, rel] : got) {
            std::string r = SafeRelPath(rel), base = r;
            for (int n = 2; !names.insert(r).second; n++) {  // (the same name twice: "a (2).jpg")
                size_t dot = base.find_last_of('.');
                size_t slash = base.find_last_of('/');
                if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) dot = base.size();
                r = base.substr(0, dot) + " (" + std::to_string(n) + ")" + base.substr(dot);
            }
            files.push_back({abs, r});
        }
    }
    if (files.empty()) {
        err = "nothing to pack";
        return false;
    }
    Utf8Locale();
    int64_t total = 0, done = 0;
    for (auto& fl : files) total += std::max<int64_t>(0, FileSize(fl.first));
    std::string part = out + ".part";
    FILE* of = OpenFile(part, "wb");
    if (!of) {
        err = "can't write " + out;
        return false;
    }
    struct archive* a = archive_write_new();
    switch (f) {
        case Format::Zip:
            archive_write_set_format_zip(a);
            archive_write_set_format_option(a, "zip", "hdrcharset", "UTF-8");
            break;
        case Format::SevenZip:
            archive_write_set_format_7zip(a);
            archive_write_set_format_option(a, "7zip", "compression", "lzma2");
            break;
        case Format::TarGz:
        case Format::TarXz:
        case Format::TarZst:
            archive_write_set_format_pax_restricted(a);
            if (f == Format::TarGz) archive_write_add_filter_gzip(a);
            else if (f == Format::TarXz) archive_write_add_filter_xz(a);
            else archive_write_add_filter_zstd(a);
            break;
    }
    archive_write_set_bytes_in_last_block(a, 1);  // (no padding at the end: zstd readers refuse it)
    if (archive_write_open(a, of, nullptr, WrWrite, WrClose) != ARCHIVE_OK) {
        err = archive_error_string(a) ? archive_error_string(a) : "can't make the archive";
        archive_write_free(a);
        fclose(of);
        RemoveFile(part);
        return false;
    }
    std::vector<char> buf(1 << 18);
    bool ok = true;
    for (auto& [abs, rel] : files) {
        FILE* in = OpenFile(abs, "rb");
        if (!in) continue;  // (gone meanwhile: left out)
        int64_t size = std::max<int64_t>(0, FileSize(abs));
        struct archive_entry* e = archive_entry_new();
#ifdef _WIN32
        {
            int wn = MultiByteToWideChar(CP_UTF8, 0, rel.c_str(), -1, nullptr, 0);
            std::wstring w(wn > 0 ? wn : 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, rel.c_str(), -1, &w[0], wn);
            archive_entry_copy_pathname_w(e, w.c_str());
        }
#else
        archive_entry_set_pathname_utf8(e, rel.c_str());
#endif
        archive_entry_set_size(e, size);
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_mtime(e, UnixMTime(abs), 0);
        if (archive_write_header(a, e) != ARCHIVE_OK) {
            err = archive_error_string(a) ? archive_error_string(a) : "can't add " + rel;
            ok = false;
        }
        archive_entry_free(e);
        size_t n;
        while (ok && (n = fread(buf.data(), 1, buf.size(), in)) > 0) {
            if (archive_write_data(a, buf.data(), n) < 0) {
                err = archive_error_string(a) ? archive_error_string(a) : "can't write";
                ok = false;
            }
            done += (int64_t)n;
            if (ok && progress && !progress(done, total, rel)) {
                err = "stopped";
                ok = false;
            }
        }
        fclose(in);
        if (!ok) break;
    }
    if (archive_write_close(a) != ARCHIVE_OK && ok) {
        err = archive_error_string(a) ? archive_error_string(a) : "can't finish the archive";
        ok = false;
    }
    archive_write_free(a);  // (closes the file)
    if (!ok) {
        RemoveFile(part);
        return false;
    }
    RemoveFile(out);
    if (!RenameFile(part, out)) {
        err = "can't write " + out;
        RemoveFile(part);
        return false;
    }
    return true;
}

}  // namespace arc
