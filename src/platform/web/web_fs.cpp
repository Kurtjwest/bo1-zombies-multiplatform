// The game folder of the browser build: /game in WasmFS, built from the list of files tools/web-server.py serves. The
// files stay on the server: each one is read from it in byte ranges as the game reads it (synchronous HTTP requests
// with a Range header, which the game's threads may make: they are Web Workers), through a cache of recently read
// blocks. Nothing of the game is copied into the browser's storage.
//
//   - folders are in memory (GameDirectory); a file the game creates is a memory file (GameMemFile)
//   - a server file (HttpFile) becomes a memory file the first time the game writes it (copied in whole, unless the
//     write truncates it first); renamed or removed like any other file
//   - what the game writes into players/ and screenshots/ is sent back to the server (PUT /files/<path>), a second or
//     two after it changes, and removed there when the game removes it: the server keeps it in its saves folder,
//     which it lays over the game folder, so every device that plays from the server has the same settings
//
// The reads go through the page's fetch worker (web_shared.h FetchTable): a request made by a game thread itself would
// keep its memory for as long as the thread runs, which is as long as the game does. (Until the worker has started,
// the first few are synchronous requests of the reading thread's own.)
//
// The WasmFS backend interface is Emscripten's own (system/lib/wasmfs): file.h, backend.h, memory_backend.h.

#include "backend.h"
#include "file.h"
#include "memory_backend.h"
#include "paths.h"
#include "special_files.h"
#include "wasmfs.h"

#include "web_shared.h"

#include <emscripten.h>
#include <emscripten/threading.h>

#include <errno.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// bo1_web_lib.js
extern "C" {
int bo1_web_http_read(const char *url, double offset, int length, void *dst);
char *bo1_web_http_get_text(const char *url, int *status);
int bo1_web_http_put(const char *url, const void *data, int length);
int bo1_web_http_delete(const char *url);
}

namespace bo1web
{
namespace
{
using namespace wasmfs;

// ---- what changed for the server (players/, screenshots/) ----------------------------------------------------------
std::atomic<uint32_t> g_writeSerial{0}; // bumped by every write to a memory file

// ---- the block cache -----------------------------------------------------------------------------------------------
// The game reads its files two ways: straight through (a fast file, 256 KB at a time) and here and there (an image in
// an .iwd archive, a few KB to a few hundred, anywhere in a file of hundreds of MB). So the cache keeps small blocks,
// and a request fetches the blocks a read needs, plus, for a file being read straight through, more of what follows
// (as much again as it has read straight through so far, up to kMaxRequest).
constexpr size_t kBlockSize = 64 << 10;     // the cache's unit (aligned in the file)
constexpr size_t kCacheBlocks = 1536;       // 96 MB of the most recently read blocks
constexpr size_t kMaxRequest = 4 << 20;     // the most one request fetches

struct Block
{
    std::vector<uint8_t> data; // (shorter than kBlockSize at the end of a file)
    bool ready = false, failed = false;
};

struct BlockCache
{
    std::mutex lock;
    std::condition_variable loaded;
    std::list<std::pair<uint64_t, std::shared_ptr<Block>>> lru; // most recent first
    std::unordered_map<uint64_t, decltype(lru)::iterator> index;
};
BlockCache g_cache;
std::atomic<uint32_t> g_nextFileId{1};

// what has been read from the server (for the log: Bridge_WebReadStats)
std::atomic<uint64_t> g_statRequests{0}, g_statBytes{0}, g_statMicros{0};

// percent-encoded, '/' kept
std::string UrlFor(const std::string &path)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string u = "files/";
    for (unsigned char c : path)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' ||
            c == '_' || c == '.' || c == '~')
            u += (char)c;
        else
        {
            u += '%';
            u += hex[c >> 4];
            u += hex[c & 15];
        }
    }
    return u;
}

// [offset, offset + length) of a server file into dst: the bytes read, or -1. Through the page's fetch worker once it
// serves the table (web_shared.h), else a synchronous request of this thread's own.
int HttpRead(const std::string &url, uint64_t offset, int length, void *dst)
{
    const double started = emscripten_get_now();
    struct Count
    {
        double started;
        int length;
        ~Count()
        {
            g_statRequests.fetch_add(1, std::memory_order_relaxed);
            g_statBytes.fetch_add((uint64_t)length, std::memory_order_relaxed);
            g_statMicros.fetch_add((uint64_t)((emscripten_get_now() - started) * 1000.0), std::memory_order_relaxed);
        }
    } count{started, length};
    FetchTable &table = Shared().fetch;
    if (!table.connected.load(std::memory_order_acquire))
        return bo1_web_http_read(url.c_str(), (double)offset, length, dst);
    // a free slot (all of them busy: another thread's reads finish first)
    FetchSlot *slot = nullptr;
    while (!slot)
    {
        for (FetchSlot &s : table.slots)
        {
            int32_t expected = FETCH_FREE;
            if (s.state.compare_exchange_strong(expected, FETCH_FILLING, std::memory_order_acq_rel))
            {
                slot = &s;
                break;
            }
        }
        if (!slot)
            emscripten_futex_wait(&table.slots[0].state, (uint32_t)table.slots[0].state.load(), 2.0);
    }
    slot->url = (int32_t)(uintptr_t)url.data();
    slot->urlLength = (int32_t)url.size();
    slot->offsetLow = (int32_t)(uint32_t)offset;
    slot->offsetHigh = (int32_t)(uint32_t)(offset >> 32);
    slot->length = length;
    slot->dst = (int32_t)(uintptr_t)dst;
    slot->result = -1;
    slot->status = 0;
    slot->state.store(FETCH_REQUESTED, std::memory_order_release);
    table.wake.fetch_add(1, std::memory_order_acq_rel);
    emscripten_futex_wake(&table.wake, 1);
    // (no time limit: the worker writes into dst until it is done, and the browser gives up on a dead server itself)
    for (;;)
    {
        const int32_t state = slot->state.load(std::memory_order_acquire);
        if (state == FETCH_DONE)
            break;
        emscripten_futex_wait(&slot->state, (uint32_t)state, 1000.0);
    }
    const int result = slot->result;
    slot->state.store(FETCH_FREE, std::memory_order_release);
    emscripten_futex_wake(&table.slots[0].state, INT32_MAX); // (a thread waiting for a free slot)
    return result;
}

// (the cache's lock held) the least recently used blocks out, down to kCacheBlocks
void EvictLocked()
{
    auto it = g_cache.lru.end();
    while (g_cache.lru.size() > kCacheBlocks && it != g_cache.lru.begin())
    {
        --it;
        if (!it->second->ready && !it->second->failed)
            continue; // (being fetched: kept until it has arrived)
        g_cache.index.erase(it->first);
        it = g_cache.lru.erase(it);
    }
}

// [offset, offset + len) of a server file into buf, from the cache or the server; if any of it has to be fetched,
// what follows it up to fetchEnd is fetched with it (read-ahead). The bytes read, or -1. (Several threads may want the
// same blocks: the first fetches them, the others wait for them.)
ssize_t ReadBlocks(uint32_t fileId, const std::string &url, uint64_t fileSize, uint64_t offset, size_t len,
                   uint64_t fetchEnd, uint8_t *buf)
{
    if (len == 0 || offset >= fileSize)
        return 0;
    len = (size_t)std::min<uint64_t>(len, fileSize - offset);
    fetchEnd = std::min(std::max(fetchEnd, offset + len), fileSize);
    const uint64_t first = offset / kBlockSize, last = (offset + len - 1) / kBlockSize;
    const uint64_t lastAhead = (fetchEnd - 1) / kBlockSize;
    auto keyOf = [fileId](uint64_t i) { return ((uint64_t)fileId << 32) | i; };

    std::vector<std::shared_ptr<Block>> blocks(last - first + 1);   // the read's
    std::vector<std::pair<uint64_t, std::shared_ptr<Block>>> mine; // the ones this thread fetches, in order
    {
        std::lock_guard<std::mutex> lk(g_cache.lock);
        auto claim = [&](uint64_t i) {
            auto b = std::make_shared<Block>();
            g_cache.lru.emplace_front(keyOf(i), b);
            g_cache.index[keyOf(i)] = g_cache.lru.begin();
            mine.emplace_back(i, b);
            return b;
        };
        for (uint64_t i = first; i <= last; ++i)
        {
            auto it = g_cache.index.find(keyOf(i));
            if (it != g_cache.index.end())
            {
                if (!it->second->second->failed)
                {
                    g_cache.lru.splice(g_cache.lru.begin(), g_cache.lru, it->second);
                    blocks[i - first] = it->second->second;
                    continue;
                }
                // (a failed fetch is tried again)
                g_cache.lru.erase(it->second);
                g_cache.index.erase(it);
            }
            blocks[i - first] = claim(i);
        }
        // what follows, with a fetch only, and up to the first block that is already here
        if (!mine.empty())
            for (uint64_t i = last + 1; i <= lastAhead && !g_cache.index.count(keyOf(i)); ++i)
                claim(i);
        EvictLocked();
    }

    // the blocks to fetch, a run of neighbours at a time (one request each)
    for (size_t r = 0; r < mine.size();)
    {
        size_t e = r + 1;
        while (e < mine.size() && mine[e].first == mine[e - 1].first + 1 && (e - r + 1) * kBlockSize <= kMaxRequest)
            ++e;
        const uint64_t start = mine[r].first * kBlockSize;
        const uint64_t end = std::min<uint64_t>((mine[e - 1].first + 1) * kBlockSize, fileSize);
        const size_t n = (size_t)(end - start);
        std::vector<uint8_t> data(n);
        int got = HttpRead(url, start, (int)n, data.data());
        // (a short read in the middle of a file is an error; one more try before giving up)
        if (got != (int)n)
            got = HttpRead(url, start, (int)n, data.data());
        if (got != (int)n)
            fprintf(stderr, "[web] could not read %s (bytes %llu-%llu) from the server\n", url.c_str(),
                    (unsigned long long)start, (unsigned long long)end);
        std::lock_guard<std::mutex> lk(g_cache.lock);
        for (size_t k = r; k < e; ++k)
        {
            Block &b = *mine[k].second;
            if (got == (int)n)
            {
                const size_t at = (size_t)(mine[k].first * kBlockSize - start);
                b.data.assign(data.begin() + at, data.begin() + std::min(n, at + kBlockSize));
                b.ready = true;
            }
            else
                b.failed = true;
        }
        g_cache.loaded.notify_all();
        r = e;
    }

    // the read's bytes (waiting for those another thread fetches)
    size_t done = 0;
    std::unique_lock<std::mutex> lk(g_cache.lock);
    for (uint64_t i = first; i <= last && done < len; ++i)
    {
        const std::shared_ptr<Block> &b = blocks[i - first];
        g_cache.loaded.wait(lk, [&] { return b->ready || b->failed; });
        if (b->failed)
            return done ? (ssize_t)done : -1;
        const size_t inBlock = (size_t)(offset + done - i * kBlockSize);
        if (inBlock >= b->data.size())
            break;
        const size_t n = std::min(len - done, b->data.size() - inBlock);
        memcpy(buf + done, b->data.data() + inBlock, n);
        done += n;
    }
    return (ssize_t)done;
}

// ---- files ---------------------------------------------------------------------------------------------------------
// a file the game made (or a server file it has written): the bytes in memory
class GameMemFile : public DataFile
{
protected:
    std::vector<uint8_t> buffer;

    int open(oflags_t) override { return 0; }
    int close() override { return 0; }
    ssize_t read(uint8_t *buf, size_t len, off_t offset) override
    {
        if (offset < 0)
            return -EINVAL;
        if ((size_t)offset >= buffer.size())
            return 0;
        len = std::min(len, buffer.size() - (size_t)offset);
        memcpy(buf, buffer.data() + offset, len);
        return (ssize_t)len;
    }
    ssize_t write(const uint8_t *buf, size_t len, off_t offset) override
    {
        if (offset < 0)
            return -EINVAL;
        if ((size_t)offset + len > buffer.size())
            buffer.resize((size_t)offset + len);
        memcpy(buffer.data() + offset, buf, len);
        Touched();
        return (ssize_t)len;
    }
    int setSize(off_t size) override
    {
        buffer.resize((size_t)std::max<off_t>(size, 0));
        Touched();
        return 0;
    }
    int flush() override { return 0; }
    off_t getSize() override { return (off_t)buffer.size(); }

    void Touched()
    {
        ++generation;
        g_writeSerial.fetch_add(1, std::memory_order_relaxed);
    }

public:
    uint32_t generation = 1; // bumped by every change (the save sync compares it)
    GameMemFile(mode_t mode, backend_t backend) : DataFile(mode, backend) {}
    // the bytes (for the save sync; the caller holds the file's lock)
    virtual bool Contents(std::vector<uint8_t> &out)
    {
        out = buffer;
        return true;
    }
};

// a file of the server's: read in blocks as the game reads it, in memory once the game writes it
class HttpFile : public GameMemFile
{
    std::string url;
    off_t remoteSize;
    uint32_t id;
    bool local = false;

    // where the file is being read straight through from, and up to (the read-ahead: WasmFS reads a file under its
    // lock, so one read at a time)
    uint64_t runStart = 0, runEnd = 0;

    ssize_t read(uint8_t *buf, size_t len, off_t offset) override
    {
        if (local)
            return GameMemFile::read(buf, len, offset);
        if (offset < 0)
            return -EINVAL;
        if (offset >= remoteSize)
            return 0;
        len = (size_t)std::min<off_t>((off_t)len, remoteSize - offset);
        if ((uint64_t)offset != runEnd)
            runStart = (uint64_t)offset;
        runEnd = (uint64_t)offset + len;
        const uint64_t ahead = std::min<uint64_t>(kMaxRequest, runEnd - runStart);
        const ssize_t got = ReadBlocks(id, url, (uint64_t)remoteSize, (uint64_t)offset, len, runEnd + ahead, buf);
        return got < 0 ? -EIO : got;
    }

    // the server's bytes into memory (the first write); count: how many of them are needed
    bool MakeLocal(off_t count)
    {
        if (local)
            return true;
        count = std::min(count, remoteSize);
        std::vector<uint8_t> data((size_t)std::max<off_t>(count, 0));
        if (count > 0 && read(data.data(), data.size(), 0) != (ssize_t)data.size())
            return false;
        buffer.swap(data);
        local = true;
        return true;
    }

    ssize_t write(const uint8_t *buf, size_t len, off_t offset) override
    {
        if (!MakeLocal(remoteSize))
            return -EIO;
        return GameMemFile::write(buf, len, offset);
    }
    int setSize(off_t size) override
    {
        if (!local && !MakeLocal(std::min(size, remoteSize)))
            return -EIO;
        return GameMemFile::setSize(size);
    }
    off_t getSize() override { return local ? (off_t)buffer.size() : remoteSize; }

public:
    HttpFile(mode_t mode, backend_t backend, const std::string &path, off_t size)
        : GameMemFile(mode, backend), url(UrlFor(path)), remoteSize(size), id(g_nextFileId.fetch_add(1))
    {
        generation = 0; // (the server has it as it is)
    }
    bool Contents(std::vector<uint8_t> &out) override
    {
        if (local)
            return GameMemFile::Contents(out);
        out.resize((size_t)remoteSize);
        return read(out.data(), out.size(), 0) == (ssize_t)out.size();
    }
};

class GameDirectory : public MemoryDirectory
{
public:
    GameDirectory(mode_t mode, backend_t backend) : MemoryDirectory(mode, backend) {}
    // a child made here rather than by the game (the server's files and folders, at startup)
    void Add(const std::string &name, std::shared_ptr<File> child)
    {
        {
            auto lock = locked();
            insertChild(name, child);
        }
        child->locked().setParent(std::static_pointer_cast<Directory>(shared_from_this()));
    }
    std::shared_ptr<File> Child(const std::string &name) { return locked().getChild(name); }
};

class GameBackend : public Backend
{
public:
    std::shared_ptr<DataFile> createFile(mode_t mode) override { return std::make_shared<GameMemFile>(mode, this); }
    std::shared_ptr<Directory> createDirectory(mode_t mode) override
    {
        return std::make_shared<GameDirectory>(mode, this);
    }
    std::shared_ptr<Symlink> createSymlink(std::string target) override
    {
        return std::make_shared<MemorySymlink>(target, this);
    }
    // /dev and /tmp in the root, as the memory backend makes them. Later Emscriptens leave that to the root's backend
    // (a virtual function of Backend, which this then overrides); Emscripten 3.1 makes them itself and never calls
    // this. (Not marked override, for the versions without it.)
    void populateRoot(Directory::Handle &lockedRoot)
    {
        if (auto dev = lockedRoot.insertDirectory("dev", S_IRUGO | S_IXUGO))
        {
            auto lockedDev = dev->locked();
            lockedDev.mountChild("null", SpecialFiles::getNull());
            lockedDev.mountChild("stdin", SpecialFiles::getStdin());
            lockedDev.mountChild("stdout", SpecialFiles::getStdout());
            lockedDev.mountChild("stderr", SpecialFiles::getStderr());
            lockedDev.mountChild("random", SpecialFiles::getRandom());
            lockedDev.mountChild("urandom", SpecialFiles::getURandom());
        }
        lockedRoot.insertDirectory("tmp", S_IRWXUGO);
    }
};

backend_t g_backend;
} // namespace

// The file system's root is one of these folders too (WasmFS asks for the root's backend as it starts:
// wasmfs_create_root_dir), so /game is an ordinary folder in it. (A folder of another backend would be a mount, which
// getcwd cannot name: the game's working directory would read as "/".)
backend_t RootBackend()
{
    if (!g_backend)
        g_backend = wasmFS.addBackend(std::make_unique<GameBackend>());
    return g_backend;
}

namespace
{

// ---- the manifest: {"version":1,"files":[["path",size,mtime],...]} -----------------------------------------------
struct ManifestEntry
{
    std::string path;
    uint64_t size = 0;
};

bool ParseManifest(const char *s, std::vector<ManifestEntry> &out)
{
    const char *p = strstr(s, "\"files\"");
    if (!p)
        return false;
    p = strchr(p, '[');
    if (!p)
        return false;
    ++p;
    for (;;)
    {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')
            ++p;
        if (*p == ']')
            return true;
        if (*p != '[')
            return false;
        ++p;
        while (*p == ' ')
            ++p;
        if (*p != '"')
            return false;
        ++p;
        ManifestEntry e;
        while (*p && *p != '"')
        {
            if (*p == '\\')
            {
                ++p;
                switch (*p)
                {
                case 'n': e.path += '\n'; break;
                case 't': e.path += '\t'; break;
                case 'u':
                {
                    // (\uXXXX: UTF-16; the server writes non-ASCII names as UTF-8 escapes, surrogate pairs combined)
                    unsigned cp = (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u')
                    {
                        const unsigned lo = (unsigned)strtoul(std::string(p + 3, 4).c_str(), nullptr, 16);
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                    if (cp < 0x80)
                        e.path += (char)cp;
                    else if (cp < 0x800)
                    {
                        e.path += (char)(0xC0 | (cp >> 6));
                        e.path += (char)(0x80 | (cp & 0x3F));
                    }
                    else if (cp < 0x10000)
                    {
                        e.path += (char)(0xE0 | (cp >> 12));
                        e.path += (char)(0x80 | ((cp >> 6) & 0x3F));
                        e.path += (char)(0x80 | (cp & 0x3F));
                    }
                    else
                    {
                        e.path += (char)(0xF0 | (cp >> 18));
                        e.path += (char)(0x80 | ((cp >> 12) & 0x3F));
                        e.path += (char)(0x80 | ((cp >> 6) & 0x3F));
                        e.path += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: e.path += *p; break;
                }
                ++p;
                continue;
            }
            e.path += *p++;
        }
        if (*p != '"')
            return false;
        ++p;
        while (*p == ' ' || *p == ',')
            ++p;
        e.size = strtoull(p, (char **)&p, 10);
        while (*p && *p != ']')
            ++p;
        if (*p != ']')
            return false;
        ++p;
        if (!e.path.empty() && e.path[0] != '/' && e.path.find("..") == std::string::npos)
            out.push_back(std::move(e));
    }
}

// the folder for rel (made as needed, folders' names in any case matching an existing one: one tree, as the game's
// lookups ignore case)
std::shared_ptr<GameDirectory> FolderFor(std::shared_ptr<GameDirectory> root, const std::string &rel,
                                         std::map<std::string, std::shared_ptr<GameDirectory>> &folders)
{
    if (rel.empty())
        return root;
    std::string key = rel;
    for (char &c : key)
        c = (char)tolower((unsigned char)c);
    auto it = folders.find(key);
    if (it != folders.end())
        return it->second;
    const size_t slash = rel.rfind('/');
    std::shared_ptr<GameDirectory> parent =
        FolderFor(root, slash == std::string::npos ? std::string() : rel.substr(0, slash), folders);
    if (!parent)
        return nullptr;
    const std::string name = slash == std::string::npos ? rel : rel.substr(slash + 1);
    std::shared_ptr<File> existing = parent->Child(name);
    std::shared_ptr<GameDirectory> dir;
    if (existing)
        dir = std::dynamic_pointer_cast<GameDirectory>(existing);
    else
    {
        dir = std::make_shared<GameDirectory>(S_IRWXU | S_IRWXG | S_IRWXO, g_backend);
        parent->Add(name, dir);
    }
    if (dir)
        folders[key] = dir;
    return dir;
}

// ---- the save sync: players/ and screenshots/ back to the server ---------------------------------------------------
const char *const kSyncedFolders[] = {"players", "screenshots"};

struct SyncState
{
    std::string root;
    // what the server has: path -> the generation sent (0: the server's own copy)
    std::map<std::string, uint32_t> sent;
};
SyncState *g_sync;

void Walk(std::shared_ptr<Directory> dir, const std::string &path, std::map<std::string, std::shared_ptr<File>> &out)
{
    Directory::MaybeEntries entries = dir->locked().getEntries();
    if (entries.getError())
        return;
    for (const Directory::Entry &e : *entries)
    {
        std::shared_ptr<File> child = dir->locked().getChild(e.name);
        if (!child)
            continue;
        const std::string p = path + "/" + e.name;
        if (std::shared_ptr<Directory> sub = child->dynCast<Directory>())
            Walk(sub, p, out);
        else if (child->is<DataFile>())
            out[p] = child;
    }
}

void SyncOnce()
{
    std::map<std::string, std::shared_ptr<File>> files;
    for (const char *folder : kSyncedFolders)
    {
        path::ParsedFile parsed = path::parseFile(g_sync->root + "/" + folder);
        if (parsed.getError())
            continue;
        std::shared_ptr<File> f = parsed.getFile();
        if (std::shared_ptr<Directory> d = f ? f->dynCast<Directory>() : nullptr)
        {
            // (the name the game folder has, which may differ in case)
            std::string name = folder;
            if (std::shared_ptr<Directory> parent = d->locked().getParent())
                name = parent->locked().getName(d);
            Walk(d, name, files);
        }
    }
    // changed and new files
    for (auto &f : files)
    {
        GameMemFile *mf = dynamic_cast<GameMemFile *>(f.second.get());
        if (!mf)
            continue;
        std::vector<uint8_t> bytes;
        uint32_t generation;
        {
            auto lock = f.second->locked();
            generation = mf->generation;
            auto it = g_sync->sent.find(f.first);
            if (it != g_sync->sent.end() && it->second == generation)
                continue;
            if (generation == 0)
            {
                g_sync->sent[f.first] = 0;
                continue;
            }
            if (!mf->Contents(bytes))
                continue;
        }
        const int status = bo1_web_http_put(UrlFor(f.first).c_str(), bytes.data(), (int)bytes.size());
        if (status >= 200 && status < 300)
            g_sync->sent[f.first] = generation;
        else
            fprintf(stderr, "[web] could not save %s on the server (HTTP %d)\n", f.first.c_str(), status);
    }
    // removed files
    for (auto it = g_sync->sent.begin(); it != g_sync->sent.end();)
    {
        if (files.count(it->first))
        {
            ++it;
            continue;
        }
        bo1_web_http_delete(UrlFor(it->first).c_str());
        it = g_sync->sent.erase(it);
    }
}

void *SyncThread(void *)
{
    uint32_t seen = g_writeSerial.load();
    for (;;)
    {
        usleep(1500 * 1000);
        const uint32_t now = g_writeSerial.load();
        if (now == seen)
            continue;
        // (until the writes stop for a moment: a file is sent whole, not halfway through being written)
        usleep(300 * 1000);
        if (g_writeSerial.load() != now)
            continue;
        seen = now;
        SyncOnce();
    }
    return nullptr;
}
} // namespace

bool MountGameFolder(const char *mountPoint, std::string *error)
{
    int status = 0;
    char *text = bo1_web_http_get_text("files/manifest.json", &status);
    if (!text)
    {
        if (error)
            *error = status ? "the server answered HTTP " + std::to_string(status) + " for files/manifest.json"
                            : "no answer from the server";
        return false;
    }
    std::vector<ManifestEntry> entries;
    const bool ok = ParseManifest(text, entries);
    free(text);
    if (!ok)
    {
        if (error)
            *error = "the server's list of files (files/manifest.json) is not one this build reads";
        return false;
    }
    if (mkdir(mountPoint, S_IRWXU | S_IRWXG | S_IRWXO) != 0 && errno != EEXIST)
    {
        if (error)
            *error = std::string("cannot make ") + mountPoint;
        return false;
    }
    path::ParsedFile parsed = path::parseFile(mountPoint);
    std::shared_ptr<GameDirectory> root =
        parsed.getError() ? nullptr : std::dynamic_pointer_cast<GameDirectory>(parsed.getFile());
    if (!root)
    {
        if (error)
            *error = std::string("cannot open ") + mountPoint;
        return false;
    }
    std::map<std::string, std::shared_ptr<GameDirectory>> folders;
    g_sync = new SyncState();
    g_sync->root = mountPoint;
    uint64_t total = 0;
    for (const ManifestEntry &e : entries)
    {
        const size_t slash = e.path.rfind('/');
        std::shared_ptr<GameDirectory> dir =
            FolderFor(root, slash == std::string::npos ? std::string() : e.path.substr(0, slash), folders);
        if (!dir)
            continue;
        const std::string name = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
        if (dir->Child(name))
            continue;
        dir->Add(name, std::make_shared<HttpFile>(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH, g_backend, e.path,
                                                  (off_t)e.size));
        total += e.size;
        for (const char *folder : kSyncedFolders)
        {
            const size_t n = strlen(folder);
            if (e.path.size() > n && strncasecmp(e.path.c_str(), folder, n) == 0 && e.path[n] == '/')
                g_sync->sent[e.path] = 0;
        }
    }
    fprintf(stderr, "[web] game folder: %zu files (%.1f GB) on the server\n", entries.size(), total / 1e9);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 << 10);
    pthread_create(&t, &attr, SyncThread, nullptr);
    pthread_attr_destroy(&attr);
    return true;
}
} // namespace bo1web

// the log: what was read from the server since the last time (after each fast file: db_file_load.cpp), which tells a
// slow load waiting for the server from one waiting for the browser
extern "C" void Bridge_WebLogReads(void)
{
    static std::mutex lock;
    static uint64_t requests, bytes, micros;
    std::lock_guard<std::mutex> lk(lock);
    const uint64_t r = bo1web::g_statRequests.load(), b = bo1web::g_statBytes.load(), m = bo1web::g_statMicros.load();
    if (r == requests)
        return;
    printf("[web] %.1f MB read from the server in %llu requests, waiting %.1f s for them\n", (b - bytes) / 1e6,
           (unsigned long long)(r - requests), (m - micros) / 1e6);
    requests = r;
    bytes = b;
    micros = m;
}

// WasmFS's hook for the root's backend, a weak definition in its wasmfs.cpp: a C++ function in namespace wasmfs in
// Emscripten 3.1, a C function in later versions. Both are here; the version's WasmFS calls its own.
namespace wasmfs
{
backend_t wasmfs_create_root_dir(void) { return bo1web::RootBackend(); }
} // namespace wasmfs
extern "C" wasmfs::backend_t wasmfs_create_root_dir(void) { return bo1web::RootBackend(); }
