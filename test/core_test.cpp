#include "JotCore.h"
#include <cassert>
#include <iostream>
#include <map>
#include <limits>

struct Memory : jot::Storage {
    std::map<std::string, std::string> files;
    bool failRead = false, failWrite = false;
    size_t cut = std::numeric_limits<size_t>::max(), writes = 0;
    jot::Read read(const char* path, std::string& out) override {
        if (failRead) return jot::Read::Error;
        auto i = files.find(path);
        if (i == files.end()) return jot::Read::Missing;
        out = i->second; return jot::Read::Ok;
    }
    bool write(const char* path, const std::string& data) override {
        ++writes;
        if (failWrite) return false;
        files[path] = data.substr(0, cut);
        return cut >= data.size();
    }
};
void type(jot::Core& c, const std::string& text) {
    for (char ch : text) assert(c.type(ch) == jot::Result::Ok);
}
int main() {
    {
        Memory fs; jot::Core c(fs); assert(c.load());
        type(c, "   "); assert(c.submit("unused") == jot::Result::Empty); assert(c.draft() == "   ");
        c.backspace(); c.backspace(); c.backspace();
        type(c, "  hello  "); assert(c.submit("one") == jot::Result::Ok);
        assert(c.pending().size() == 1 && c.pending()[0].text == "hello" && c.draft().empty());
        jot::Core restart(fs); assert(restart.load()); assert(restart.pending()[0].id == "one");
        auto batch = c.pending(); type(c, "new during POST"); assert(c.submit("two") == jot::Result::Ok);
        assert(c.acknowledge(batch)); assert(c.pending().size() == 1 && c.pending()[0].id == "two");
        jot::Core reboot(fs); assert(reboot.load()); assert(reboot.pending()[0].id == "two");
    }
    {
        Memory fs; jot::Core c(fs); assert(c.load());
        type(c, std::string(512, 'a')); assert(c.type('b') == jot::Result::Limit); assert(c.draft().size() == 512);
        fs.failWrite = true;
        assert(c.submit("one") == jot::Result::StorageError); assert(c.draft().size() == 512 && c.pending().empty());
        fs.failWrite = false; assert(c.submit("one") == jot::Result::Ok);
        fs.failWrite = true; assert(!c.acknowledge(c.pending())); assert(c.pending().size() == 1);
        fs.failWrite = false; jot::Core restart(fs); assert(restart.load()); assert(restart.pending().size() == 1);
    }
    {
        Memory fs; jot::Core c(fs); assert(c.load());
        for (size_t i = 0; i < 100; ++i) { type(c, std::string(512, 'n')); assert(c.submit(std::to_string(i)) == jot::Result::Ok); }
        type(c, "retain me"); assert(c.submit("full") == jot::Result::Full);
        assert(c.draft() == "retain me" && c.pending().size() == 100);
        jot::Core reboot(fs); assert(reboot.load()); assert(reboot.pending().size() == 100);
        for (const auto& m : reboot.pending()) assert(m.text.size() == 512);
    }
    {
        Memory seed; jot::Core c(seed); assert(c.load()); type(c, "safe"); assert(c.submit("one") == jot::Result::Ok);
        // Interrupt the inactive-slot write at every byte boundary of the next snapshot.
        Memory full = seed; jot::Core next(full); assert(next.load()); type(next, "second"); assert(next.submit("two") == jot::Result::Ok);
        const size_t size = full.files.at("/queue-b.bin").size();
        for (size_t cut = 0; cut < size; ++cut) {
            Memory fs = seed; jot::Core writing(fs); assert(writing.load()); type(writing, "second"); fs.cut = cut;
            assert(writing.submit("two") == jot::Result::StorageError); assert(writing.draft() == "second");
            fs.cut = std::numeric_limits<size_t>::max(); jot::Core reboot(fs); assert(reboot.load());
            assert(reboot.pending().size() == 1 && reboot.pending()[0].id == "one");
        }
        for (size_t cut = 0; cut < 20; ++cut) {
            Memory fs = seed; jot::Core writing(fs); assert(writing.load()); fs.cut = cut;
            assert(!writing.acknowledge(writing.pending())); fs.cut = std::numeric_limits<size_t>::max();
            jot::Core reboot(fs); assert(reboot.load()); assert(reboot.pending().size() == 1);
        }
    }
    {
        Memory fs; fs.files["/messages.tsv"] = "old-id\told text\r\n";
        jot::Core c(fs); assert(c.load()); assert(c.pending()[0].id == "old-id");
        assert(c.acknowledge(c.pending()));
        jot::Core reboot(fs); assert(reboot.load()); assert(reboot.pending().empty()); // no legacy resurrection
        fs.files["/queue-a.bin"] = "corrupt"; fs.files["/queue-b.bin"] = "corrupt";
        auto writes = fs.writes; jot::Core broken(fs); assert(!broken.load());
        type(broken, "keep"); assert(broken.submit("id") == jot::Result::StorageError);
        assert(broken.draft() == "keep" && fs.writes == writes);
    }
    {
        Memory fs; fs.files["/messages.tsv"] = "bad legacy data\n";
        jot::Core c(fs); assert(!c.load()); assert(fs.writes == 0);
        Memory io; io.failRead = true; jot::Core unavailable(io); assert(!unavailable.load()); assert(io.writes == 0);
    }
    {
        const auto json = jot::payload("dev", {{"id", "quote\" slash\\ newline\n"}});
        assert(json == "{\"device_id\":\"dev\",\"messages\":[{\"id\":\"id\",\"text\":\"quote\\\" slash\\\\ newline\\u000a\"}]}");
        jot::Hold h;
        assert(!h.update(true, UINT32_MAX - 1000, 3000));
        assert(!h.update(true, 1000, 3000)); assert(h.update(true, 2000, 3000));
        assert(!h.update(true, 6000, 3000)); assert(!h.update(false, 6001, 3000));
        assert(h.update(true, 7000, 0)); assert(!h.update(true, 7001, 0));
    }
    std::cout << "Jot core tests passed: limits, persistence, every torn-write boundary, migration, acknowledgement, JSON, holds\n";
}
