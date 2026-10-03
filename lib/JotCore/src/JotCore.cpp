#include "JotCore.h"
#include <algorithm>
#include <limits>

namespace jot {
namespace {
const char* slots[] = {"/queue-a.bin", "/queue-b.bin"};
uint32_t hash(const std::string& s, size_t length = std::string::npos) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < std::min(s.size(), length); ++i) { h ^= static_cast<unsigned char>(s[i]); h *= 16777619u; }
    return h;
}
void number(std::string& out, uint64_t n, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) out.push_back(static_cast<char>(n >> (i * 8)));
}
bool number(const std::string& s, size_t& pos, uint64_t& n, unsigned bytes) {
    if (pos + bytes > s.size()) return false;
    n = 0;
    for (unsigned i = 0; i < bytes; ++i) n |= uint64_t(static_cast<unsigned char>(s[pos++])) << (8 * i);
    return true;
}
bool contains(const std::vector<Message>* list, const Message& m) {
    if (list) for (const auto& d : *list) if (m.id == d.id) return true;
    return false;
}
std::string encode(uint64_t generation, const std::vector<Message>& messages,
                   const std::vector<Message>* exclude) {
    std::string out = "JOT1";
    size_t count = 0;
    size_t bytes = 20;
    for (const auto& m : messages) if (!contains(exclude, m)) {
        ++count; bytes += 4 + m.id.size() + m.text.size();
    }
    out.reserve(bytes);
    number(out, generation, 8); number(out, count, 4);
    for (const auto& m : messages) {
        if (contains(exclude, m)) continue;
        number(out, m.id.size(), 2); number(out, m.text.size(), 2);
        out += m.id; out += m.text;
    }
    number(out, hash(out), 4);
    return out;
}
bool decode(const std::string& data, uint64_t& generation, std::vector<Message>& messages) {
    if (data.size() < 20 || data.compare(0, 4, "JOT1") != 0) return false;
    size_t end = data.size() - 4, p = end;
    uint64_t checksum = 0, count = 0;
    if (!number(data, p, checksum, 4) || checksum != hash(data, end)) return false;
    p = 4;
    if (!number(data, p, generation, 8) || generation == 0 ||
        !number(data, p, count, 4) || count > maxQueue) return false;
    messages.clear();
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t id = 0, text = 0;
        if (!number(data, p, id, 2) || !number(data, p, text, 2) ||
            !id || id > 128 || !text || text > maxText || p + id + text > end) return false;
        Message m{data.substr(p, id), data.substr(p + id, text)};
        for (const auto& old : messages) if (old.id == m.id) return false;
        messages.push_back(m); p += id + text;
    }
    return p == end;
}
std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::string escape(const std::string& s) {
    std::string out;
    const char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += c;
    }
    return out;
}
}
bool Core::load() {
    ready_ = false; messages_.clear(); generation_ = 0;
    bool found = false;
    for (const char* slot : slots) {
        std::string data;
        Read r = storage_.read(slot, data);
        // An I/O error is not proof that the other slot is newer or complete.
        if (r == Read::Error) return false;
        if (r == Read::Missing) continue;
        found = true;
        uint64_t g = 0; std::vector<Message> next;
        if (decode(data, g, next) && g > generation_) { generation_ = g; messages_ = std::move(next); }
    }
    if (found) { ready_ = generation_ != 0; return ready_; }
    // Import the old queue only when no journal exists; never revive acknowledged legacy notes.
    std::string legacy;
    Read r = storage_.read("/messages.tsv", legacy);
    if (r == Read::Error) return false;
    if (r == Read::Ok) {
        size_t pos = 0;
        while (pos < legacy.size()) {
            size_t e = legacy.find('\n', pos);
            if (e == std::string::npos) e = legacy.size();
            std::string line = trim(legacy.substr(pos, e - pos)); pos = e + 1;
            if (line.empty()) continue;
            size_t tab = line.find('\t');
            if (tab == std::string::npos || !tab || tab > 128 || tab + 1 == line.size() ||
                line.size() - tab - 1 > maxText || messages_.size() >= maxQueue) return false;
            Message m{line.substr(0, tab), line.substr(tab + 1)};
            for (const auto& old : messages_) if (old.id == m.id) return false;
            messages_.push_back(m);
        }
    }
    // Establish a valid empty journal too: subsequent corruption cannot look like first boot.
    ready_ = true;
    if (!commit(messages_)) { ready_ = false; return false; }
    return true;
}
bool Core::commit(const std::vector<Message>& next, const std::vector<Message>* exclude) {
    if (!ready_ || generation_ == std::numeric_limits<uint64_t>::max()) return false;
    const auto g = generation_ + 1;
    const auto data = encode(g, next, exclude);
    const char* path = slots[g % 2];
    if (!storage_.write(path, data)) return false;
    if (!storage_.verify(path, data)) return false;
    generation_ = g;
    if (&next != &messages_) messages_ = next;
    return true;
}
Result Core::type(char c) {
    if (c < 32 || c > 126) return Result::Invalid;
    if (draft_.size() >= maxText) return Result::Limit;
    draft_ += c; return Result::Ok;
}
void Core::backspace() { if (!draft_.empty()) draft_.pop_back(); }
Result Core::submit(const std::string& id) {
    const auto text = trim(draft_);
    if (text.empty()) return Result::Empty;
    if (!ready_ || id.empty() || id.size() > 128) return Result::StorageError;
    if (messages_.size() >= maxQueue) return Result::Full;
    for (const auto& m : messages_) if (m.id == id) return Result::Invalid;
    // Stage one new note, not a second copy of the entire queue.
    messages_.push_back({id, text});
    if (!commit(messages_)) { messages_.pop_back(); return Result::StorageError; }
    draft_.clear(); return Result::Ok;
}
bool Core::acknowledge(const std::vector<Message>& delivered) {
    bool changed = false;
    for (const auto& m : messages_) if (contains(&delivered, m)) changed = true;
    if (!changed) return true;
    // Persist the filtered snapshot before removing anything from the live queue.
    if (!commit(messages_, &delivered)) return false;
    messages_.erase(std::remove_if(messages_.begin(), messages_.end(), [&](const Message& m) {
        return contains(&delivered, m);
    }), messages_.end());
    return true;
}
std::string payload(const std::string& device, const std::vector<Message>& messages) {
    std::string out = "{\"device_id\":\"" + escape(device) + "\",\"messages\":[";
    bool first = true;
    for (const auto& m : messages) {
        if (!first) out += ',';
        first = false;
        out += "{\"id\":\"" + escape(m.id) + "\",\"text\":\"" + escape(m.text) + "\"}";
    }
    return out + "]}";
}
bool Hold::update(bool held, uint32_t now, uint32_t duration) {
    if (!held) { active_ = fired_ = false; return false; }
    if (!active_) { active_ = true; start_ = now; }
    if (!fired_ && uint32_t(now - start_) >= duration) { fired_ = true; return true; }
    return false;
}
} // namespace jot
