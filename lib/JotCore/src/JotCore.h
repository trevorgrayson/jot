#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace jot {
constexpr size_t maxText = 512;
constexpr size_t maxQueue = 100;
struct Message { std::string id, text; };
// Adapters distinguish absent files from read errors. Writes must flush before returning.
enum class Read { Missing, Ok, Error };
struct Storage {
    virtual ~Storage() = default;
    virtual Read read(const char* path, std::string& data) = 0;
    virtual bool write(const char* path, const std::string& data) = 0;
    virtual bool verify(const char* path, const std::string& expected) {
        std::string actual;
        return read(path, actual) == Read::Ok && actual == expected;
    }
};
enum class Result { Ok, Empty, Full, Limit, StorageError, Invalid };
class Core {
public:
    explicit Core(Storage& storage) : storage_(storage) {}
    bool load();
    Result type(char c);
    void backspace();
    Result submit(const std::string& id);
    bool acknowledge(const std::vector<Message>& delivered);
    const std::string& draft() const { return draft_; }
    const std::vector<Message>& pending() const { return messages_; }
    bool ready() const { return ready_; }
private:
    bool commit(const std::vector<Message>& next, const std::vector<Message>* exclude = nullptr);
    Storage& storage_;
    std::string draft_;
    std::vector<Message> messages_;
    uint64_t generation_ = 0;
    bool ready_ = false;
};
std::string payload(const std::string& device, const std::vector<Message>& messages);
// One chord/hold action per physical hold, including across mode transitions.
class Hold {
public:
    bool update(bool held, uint32_t now, uint32_t duration);
private:
    bool active_ = false, fired_ = false;
    uint32_t start_ = 0;
};
} // namespace jot
