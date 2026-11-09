#pragma once
#include "sql.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <system_error>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <variant>

namespace mini {
enum class Type : uint8_t { Bigint, Varchar, Vector };
using Value = std::variant<int64_t, std::string, std::vector<float>>;

struct Column {
    std::string name;
    Type type;
    uint32_t size = 0;
};

template <class T> void put(std::string& bytes, T value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof value);
}
inline void put_string(std::string& bytes, std::string_view value) {
    if (value.size() > UINT32_MAX) invalid("String too large");
    put(bytes, static_cast<uint32_t>(value.size()));
    bytes.append(value);
}

class Reader {
    std::string_view bytes;
public:
    explicit Reader(std::string_view s) : bytes(s) {}
    std::string_view take(size_t n) {
        if (n > bytes.size()) throw std::runtime_error("Invalid storage record");
        auto s = bytes.substr(0, n);
        bytes.remove_prefix(n);
        return s;
    }
    template <class T> T get() {
        T result;
        auto s = take(sizeof result);
        std::memcpy(&result, s.data(), sizeof result);
        return result;
    }
    std::string_view string() { return take(get<uint32_t>()); }
    bool empty() const { return bytes.empty(); }
};

struct Table {
    uint32_t id;
    std::vector<Column> columns;
    size_t primary;
    std::string index;
    // A zero offset in pending represents a tombstone.
    std::map<int64_t, uint64_t> rows;
    std::map<int64_t, uint64_t> pending;

    size_t column(const std::string& name) const {
        for (size_t i = 0; i < columns.size(); ++i) if (columns[i].name == name) return i;
        throw Error("UNKNOWN_COLUMN", "Unknown column " + name);
    }
    uint64_t find(int64_t key) const {
        auto change = pending.find(key);
        if (change != pending.end()) return change->second;
        auto row = rows.find(key);
        return row == rows.end() ? 0 : row->second;
    }
    template <class Visitor>
    void visit(Visitor visitor, int64_t low, int64_t high, bool descending) const {
        auto begin = [low](const auto& index) {
            return low == INT64_MIN ? index.begin() : index.lower_bound(low);
        };
        auto end = [high](const auto& index) {
            return high == INT64_MAX ? index.end() : index.upper_bound(high);
        };
        auto walk = [&](auto row, auto row_end, auto change, auto change_end, auto before) {
            while (row != row_end || change != change_end) {
                if (change == change_end || (row != row_end && before(row->first, change->first))) {
                    if (!visitor(row->first, row->second)) return;
                    ++row;
                } else {
                    if (row != row_end && row->first == change->first) ++row;
                    if (change->second && !visitor(change->first, change->second)) return;
                    ++change;
                }
            }
        };
        if (descending) {
            walk(std::make_reverse_iterator(end(rows)), std::make_reverse_iterator(begin(rows)),
                 std::make_reverse_iterator(end(pending)), std::make_reverse_iterator(begin(pending)),
                 std::greater<int64_t>{});
        } else {
            walk(begin(rows), end(rows), begin(pending), end(pending), std::less<int64_t>{});
        }
    }
};

inline void io_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

class Log {
    static constexpr std::string_view magic = "MVDB0001";
    static constexpr uint64_t header_size = 16;
    int fd = -1;
    char* mapped = nullptr;
    uint64_t map_size = 0;
    uint64_t end = 0;
    uint64_t frame = 0;
    std::string buffered;
    static constexpr size_t buffer_limit = 256 * 1024;

    void flush() {
        if (buffered.empty()) return;
        write(end - buffered.size(), buffered);
        buffered.clear();
    }

    void unmap() {
        if (mapped) munmap(mapped, map_size);
        mapped = nullptr;
        map_size = 0;
    }
    void write(uint64_t offset, std::string_view bytes) {
        while (!bytes.empty()) {
            auto n = pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) io_error("write data");
            offset += n;
            bytes.remove_prefix(n);
        }
    }
    void truncate(uint64_t size) {
        unmap();
        if (ftruncate(fd, static_cast<off_t>(size)) < 0) io_error("truncate data");
        end = size;
    }

public:
    explicit Log(const std::string& dir) {
        std::filesystem::create_directories(dir);
        auto path = std::filesystem::path(dir) / "records.bin";
        fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) io_error("open data");
        try {
            struct stat info {};
            if (fstat(fd, &info) < 0) io_error("stat data");
            end = info.st_size;
            if (end < magic.size()) {
                write(0, magic);
                end = magic.size();
                sync();
                int directory = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (directory < 0) io_error("open data directory");
                int result = fsync(directory);
                int saved = errno;
                close(directory);
                if (result < 0 && saved != EINVAL) {
                    errno = saved;
                    io_error("sync data directory");
                }
            }
            if (view(0, magic.size()) != magic) throw std::runtime_error("Unknown data format");
        } catch (...) {
            unmap();
            close(fd);
            throw;
        }
    }
    ~Log() { unmap(); close(fd); }
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void sync() {
#ifdef __APPLE__
        while (fsync(fd) < 0) if (errno != EINTR) io_error("sync data");
#else
        while (fdatasync(fd) < 0) if (errno != EINTR) io_error("sync data");
#endif
    }
    std::string_view view(uint64_t offset, size_t count) {
        if (offset > end || count > end - offset) throw std::runtime_error("Read past log end");
        if (offset + count > map_size) {
            unmap();
            if (offset + count > end - buffered.size()) flush();
            uint64_t physical_size = end - buffered.size();
            auto ptr = mmap(nullptr, physical_size, PROT_READ, MAP_SHARED, fd, 0);
            if (ptr == MAP_FAILED) io_error("map data");
            mapped = static_cast<char*>(ptr);
            map_size = physical_size;
        }
        return {mapped + offset, count};
    }
    std::string_view record(uint64_t offset) {
        auto size = Reader(view(offset, 4)).get<uint32_t>();
        return view(offset + 4, size);
    }
    template <class Apply> void recover(Apply apply) {
        uint64_t offset = magic.size();
        while (end - offset >= header_size) {
            Reader header(view(offset, header_size));
            uint64_t length = header.get<uint64_t>();
            uint64_t inverse = header.get<uint64_t>();
            if (!length || inverse != ~length || length > end - offset - header_size) break;
            offset += header_size;
            uint64_t limit = offset + length;
            while (offset < limit) {
                if (limit - offset < 4) throw std::runtime_error("Invalid frame");
                auto bytes = record(offset);
                if (bytes.size() > limit - offset - 4) throw std::runtime_error("Invalid frame record");
                apply(offset, bytes);
                offset += 4 + bytes.size();
            }
        }
        if (offset != end) {
            truncate(offset);
            sync();
        } else unmap();
    }
    uint64_t size() const { return end; }
    void begin() {
        frame = end;
        buffered.assign(header_size, '\0');
        end += header_size;
    }
    uint64_t append(std::string_view bytes) {
        if (bytes.size() > UINT32_MAX) invalid("Storage record too large");
        uint32_t count = static_cast<uint32_t>(bytes.size());
        size_t total = sizeof count + bytes.size();
        if (buffered.size() + total > buffer_limit) flush();
        uint64_t offset = end;
        if (total > buffer_limit) {
            write(end, {reinterpret_cast<const char*>(&count), sizeof count});
            write(end + sizeof count, bytes);
        } else {
            put(buffered, count);
            buffered.append(bytes);
        }
        end += total;
        return offset;
    }
    void rewind(uint64_t offset) {
        uint64_t physical_size = end - buffered.size();
        if (offset >= physical_size) {
            buffered.resize(static_cast<size_t>(offset - physical_size));
            end = offset;
        } else {
            buffered.clear();
            truncate(offset);
        }
    }
    void commit() {
        uint64_t length = end - frame - header_size;
        if (!length) { rollback(); return; }
        std::string header;
        put(header, length);
        put(header, ~length);
        if (end - buffered.size() == frame) {
            // Small transactions fit in one contiguous write. A short write still fails
            // recovery's frame-length check until the entire payload reaches the file.
            buffered.replace(0, header_size, header);
            flush();
        } else {
            flush();
            write(frame, header);
        }
        sync();
    }
    void rollback() { rewind(frame); }
};
} // namespace mini
