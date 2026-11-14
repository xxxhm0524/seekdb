#include "storage.hpp"
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <thread>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mvd {
uint64_t checksum(const void* raw, size_t size) {
    const auto* data = static_cast<const unsigned char*>(raw);
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) { h ^= data[i]; h *= 1099511628211ULL; }
    return h;
}
uint64_t read_le(const unsigned char* p, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
void write_le(unsigned char* p, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
static void io_failure(const char* op) { throw IoError(std::string(op) + ": " + std::strerror(errno)); }
File::File(const std::filesystem::path& path) {
#ifdef _WIN32
    fd_ = _wopen(path.wstring().c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
#endif
    if (fd_ < 0) io_failure("open database");
#ifndef _WIN32
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        if ((errno != EWOULDBLOCK && errno != EINTR) || std::chrono::steady_clock::now() >= deadline) {
            ::close(fd_); fd_ = -1; io_failure("lock database");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
}
File::~File() {
    if (fd_ < 0) return;
#ifdef _WIN32
    _close(fd_);
#else
    ::close(fd_);
#endif
}
void File::read(uint64_t offset, void* data, size_t n) const {
    auto* p = static_cast<unsigned char*>(data);
    while (n) {
        size_t chunk = std::min(n, size_t(1) << 30);
#ifdef _WIN32
        if (_lseeki64(fd_, static_cast<int64_t>(offset), SEEK_SET) < 0) io_failure("seek");
        int got = _read(fd_, p, static_cast<unsigned>(chunk));
#else
        ssize_t got = ::pread(fd_, p, chunk, static_cast<off_t>(offset));
#endif
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) io_failure("read");
        if (!got) throw IoError("unexpected end of database file");
        offset += got; p += got; n -= got;
    }
}
void File::write(uint64_t offset, const void* data, size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    while (n) {
        size_t chunk = std::min(n, size_t(1) << 30);
#ifdef _WIN32
        if (_lseeki64(fd_, static_cast<int64_t>(offset), SEEK_SET) < 0) io_failure("seek");
        int wrote = _write(fd_, p, static_cast<unsigned>(chunk));
#else
        ssize_t wrote = ::pwrite(fd_, p, chunk, static_cast<off_t>(offset));
#endif
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) io_failure("write");
        offset += wrote; p += wrote; n -= wrote;
    }
}
uint64_t File::size() const {
#ifdef _WIN32
    struct _stati64 s{};
    if (_fstati64(fd_, &s)) io_failure("stat");
#else
    struct stat s{};
    if (::fstat(fd_, &s)) io_failure("stat");
#endif
    return static_cast<uint64_t>(s.st_size);
}
void File::truncate(uint64_t size) {
#ifdef _WIN32
    if (_chsize_s(fd_, size)) io_failure("truncate");
#else
    if (::ftruncate(fd_, static_cast<off_t>(size))) io_failure("truncate");
#endif
}
void File::sync() {
#ifdef _WIN32
    if (_commit(fd_)) io_failure("sync");
#else
    int rc;
    do { rc = ::fdatasync(fd_); } while (rc < 0 && errno == EINTR);
    if (rc) io_failure("sync");
#endif
}
static size_t cache_pages() {
    size_t mib = 4;
    if (const char* v = std::getenv("MVD_PAGE_CACHE_MIB")) {
        try { mib = std::stoull(v); } catch (...) { fail("invalid_config", "invalid page cache size"); }
        if (mib > 64) fail("invalid_config", "page cache maximum is 64 MiB");
    }
    return std::max(size_t(8), mib * 1024 * 1024 / PAGE_SIZE);
}
Storage::Storage(const std::filesystem::path& dir) : file_(dir / "database.mvd"), cache_pages_(cache_pages()) {
    const uint64_t file_size = file_.size();
    bool found = false, has_signature = false;
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (file_size < (slot + 1) * PAGE_SIZE) continue;
        Page h{}; file_.read(slot * PAGE_SIZE, h.data(), h.size());
        if (std::memcmp(h.data(), "MVDCP001", 8)) continue;
        has_signature = true;
        if (read_le(h.data() + 4088, 8) != checksum(h.data(), 4088)) continue;
        uint64_t gen = read_le(h.data() + 8, 8), off = read_le(h.data() + 16, 8);
        uint64_t len = read_le(h.data() + 24, 8), end = read_le(h.data() + 32, 8);
        if (off < 8192 || !len || len > (64ULL << 20) || end > file_size || off > end || len > end - off) continue;
        std::string bytes(static_cast<size_t>(len), '\0'); file_.read(off, bytes.data(), bytes.size());
        if (checksum(bytes.data(), bytes.size()) != read_le(h.data() + 40, 8)) continue;
        try {
            auto catalog = catalog_from_json(Json::parse(bytes));
            if (!found || gen > generation_) {
                committed = std::move(catalog); generation_ = gen; end_ = durable_end_ = end; found = true;
            }
        } catch (const std::exception&) { continue; }
    }
    if (!found) {
        // An incomplete first initialization has never acknowledged any SQL.
        if (has_signature) throw IoError("no valid committed database root");
        file_.truncate(PAGE_SIZE * 2); end_ = durable_end_ = PAGE_SIZE * 2;
        commit({});
    } else file_.truncate(durable_end_);
}
void Storage::flush_page(uint64_t offset, Cached& p) {
    if (!p.dirty) return;
    write_le(p.page.data() + 4088, checksum(p.page.data(), 4088), 8);
    file_.write(offset, p.page.data(), p.page.size()); p.dirty = false;
}
void Storage::evict() {
    while (pages_.size() >= cache_pages_) {
        uint64_t offset = lru_.back(); auto it = pages_.find(offset);
        flush_page(offset, it->second); pages_.erase(it); lru_.pop_back();
    }
}
Page Storage::page(uint64_t offset) {
    auto it = pages_.find(offset);
    if (it != pages_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru); return it->second.page;
    }
    if (offset < 8192 || offset % PAGE_SIZE || offset > end_ || PAGE_SIZE > end_ - offset) throw IoError("invalid page reference");
    Page p{}; file_.read(offset, p.data(), p.size());
    if (read_le(p.data() + 4088, 8) != checksum(p.data(), 4088)) throw IoError("page checksum mismatch");
    evict(); lru_.push_front(offset); pages_.emplace(offset, Cached{p, false, lru_.begin()}); return p;
}
void Storage::set_page(uint64_t offset, const Page& p) {
    auto it = pages_.find(offset);
    if (it != pages_.end()) {
        it->second.page = p; it->second.dirty = true; lru_.splice(lru_.begin(), lru_, it->second.lru);
    } else {
        evict(); lru_.push_front(offset); pages_.emplace(offset, Cached{p, true, lru_.begin()});
    }
}
uint64_t Storage::allocate_page() {
    uint64_t offset = (end_ + PAGE_SIZE - 1) & ~(uint64_t(PAGE_SIZE) - 1);
    if (offset > uint64_t(INT64_MAX) - PAGE_SIZE) fail("resource_limit", "database file too large");
    end_ = offset + PAGE_SIZE; return offset;
}
Ref Storage::append(const std::string& bytes) {
    if (bytes.size() > UINT32_MAX || end_ > uint64_t(INT64_MAX) - bytes.size()) fail("resource_limit", "record too large");
    Ref r{end_, static_cast<uint32_t>(bytes.size())};
    file_.write(end_, bytes.data(), bytes.size()); end_ += bytes.size(); return r;
}
std::string Storage::read(Ref r) const {
    if (r.offset < 8192 || r.offset > end_ || r.length > end_ - r.offset) throw IoError("invalid record reference");
    std::string bytes(r.length, '\0'); file_.read(r.offset, bytes.data(), bytes.size()); return bytes;
}
void Storage::rewind(uint64_t mark) {
    if (mark < durable_end_ || mark > end_) throw IoError("invalid statement rollback boundary");
    for (auto it = pages_.begin(); it != pages_.end();) {
        if (it->first >= mark) { lru_.erase(it->second.lru); it = pages_.erase(it); } else ++it;
    }
    file_.truncate(mark); end_ = mark;
}
void Storage::commit(const Catalog& catalog) {
    // Allocate/copy everything that can fail before publishing the durable root.
    Catalog next = catalog;
    std::string bytes = catalog_json(next).dump();
    Ref ref = append(bytes);
    for (auto& [offset, cached] : pages_) flush_page(offset, cached);
    file_.sync();
    Page header{}; std::memcpy(header.data(), "MVDCP001", 8);
    write_le(header.data() + 8, generation_ + 1, 8);
    write_le(header.data() + 16, ref.offset, 8); write_le(header.data() + 24, ref.length, 8);
    write_le(header.data() + 32, end_, 8); write_le(header.data() + 40, checksum(bytes.data(), bytes.size()), 8);
    write_le(header.data() + 4088, checksum(header.data(), 4088), 8);
    file_.write(((generation_ + 1) % 2) * PAGE_SIZE, header.data(), header.size());
    file_.sync();
    ++generation_; durable_end_ = end_; committed.swap(next);
}
}
