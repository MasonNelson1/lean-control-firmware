// g++ -std=c++17 -Wall -Wextra -Werror -I src tests/native/config_exhaustive_test.cpp src/config/Registry.cpp src/config/Persistence.cpp src/config/Protocol.cpp -o .pio/config_exhaustive_test.exe
#include "config/Protocol.h"
#include <array>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace config;

static int passed = 0, failed = 0;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

static void test(const std::string &name, const std::function<void()> &body) {
    try {
        body();
        ++passed;
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception &error) {
        ++failed;
        std::cout << "FAIL " << name << " -- " << error.what() << '\n';
    }
}

class Memory : public Storage {
public:
    std::array<uint8_t, Persistence::StorageSize> data{};
    int budget = -1;
    size_t writes = 0;
    Memory() { data.fill(0xff); }
    uint8_t read(size_t address) const override { return data.at(address); }
    void update(size_t address, uint8_t value) override {
        if (data.at(address) == value || budget == 0) return;
        if (budget > 0) --budget;
        data[address] = value;
        ++writes;
    }
};

template<typename T> static bool put(Field &field, T value) {
    return field.update(reinterpret_cast<const uint8_t *>(&value), sizeof(value));
}

template<typename T> static void scalars(const std::string &name, Type type) {
    const T minimum = std::is_signed_v<T> ? T(-10) : T(2);
    const T maximum = 10;
    using Action = std::function<void(ConfigField<T> &)>;
    const std::vector<std::pair<std::string, Action>> cases = {
        {"minimum accepted", [=](auto &f) { CHECK(put(f, minimum)); CHECK(f.get() == minimum); }},
        {"maximum accepted", [=](auto &f) { CHECK(put(f, maximum)); CHECK(f.get() == maximum); }},
        {"below minimum rejected", [=](auto &f) { CHECK(!put(f, T(minimum - 1))); CHECK(f.get() == T(5)); }},
        {"above maximum rejected", [=](auto &f) { CHECK(!put(f, T(maximum + 1))); CHECK(f.get() == T(5)); }},
        {"short encoding rejected", [](auto &f) { uint8_t raw[sizeof(T)]{}; CHECK(!f.update(raw, sizeof(T) - 1)); }},
        {"trailing bytes rejected", [](auto &f) { uint8_t raw[sizeof(T) + 1]{}; CHECK(!f.update(raw, sizeof(raw))); }},
        {"reset restores default", [=](auto &f) { CHECK(put(f, maximum)); f.reset(); CHECK(f.get() == T(5)); }},
        {"typed encoding and metadata", [=](auto &f) {
            uint8_t raw[32]{};
            Writer value(raw, sizeof(raw));
            f.value(value);
            CHECK(value.good() && value.size() == sizeof(T) && read<T>(raw) == T(5));
            Writer metadata(raw, sizeof(raw));
            f.metadata(metadata);
            CHECK(metadata.size() == 3 * sizeof(T));
            CHECK(read<T>(raw) == T(5) && read<T>(raw + sizeof(T)) == minimum);
            CHECK(read<T>(raw + 2 * sizeof(T)) == maximum && f.type == type);
        }}
    };
    for (const auto &entry : cases) test(name + " / " + entry.first, [&] {
        Registry registry;
        Group group(registry, 1, "group");
        ConfigField<T> field(registry, 1, {group, "value", "help"}, T(5), minimum, maximum);
        entry.second(field);
    });
}

static void profiles() {
    const float defaults[] = {.5f, .25f};
    const Parameter bounds[] = {{"x", 0, 1}, {"y", 0, 1}};
    const std::vector<std::pair<std::string, std::array<float, 2>>> cases = {
        {"zero input", {0, .5f}}, {"unit input", {1, .5f}},
        {"negative input", {-.1f, .5f}}, {"zero output", {.5f, 0}},
        {"unit output", {.5f, 1}}, {"negative output", {.5f, -.1f}},
        {"nan input", {std::numeric_limits<float>::quiet_NaN(), .5f}},
        {"infinite output", {.5f, std::numeric_limits<float>::infinity()}},
        {"attenuation", {.6f, .2f}}, {"amplification", {.2f, .8f}},
        {"linear", {.5f, .5f}}, {"small positive knee", {.001f, .001f}}
    };
    for (size_t i = 0; i < cases.size(); ++i) test("power profile / " + cases[i].first, [&] {
        Registry registry;
        Group group(registry, 1, "curves");
        ProfileField<2> field(registry, 1, {group, "curve", ""}, Editor::SignedPower, defaults, bounds);
        uint8_t raw[9];
        Writer writer(raw, sizeof(raw));
        writer.put(uint8_t{1});
        writer.bytes(cases[i].second.data(), 8);
        CHECK(field.update(raw, sizeof(raw)) == (i >= 8));
        if (i < 8) CHECK(field.get()[0] == .5f && field.get()[1] == .25f);
    });
    for (const uint8_t tag : {uint8_t{0}, uint8_t{2}, uint8_t{255}}) test("profile editor mismatch " + std::to_string(tag), [=] {
        Registry r; Group g(r, 1, "curves");
        ProfileField<2> f(r, 1, {g, "curve", ""}, Editor::SignedPower, defaults, bounds);
        uint8_t raw[9] = {tag};
        CHECK(!f.update(raw, sizeof(raw)));
    });
    for (const float midpoint : {0.0f, .5f, 1.0f}) test("piecewise midpoint " + std::to_string(midpoint), [=] {
        Registry r; Group g(r, 1, "curves");
        const float defaults3[] = {0, 0, .5f, .5f, 1, 1};
        const Parameter b[] = {{"x0", 0, 0}, {"y0", 0, 1}, {"x1", 0, 1}, {"y1", 0, 1}, {"x2", 1, 1}, {"y2", 0, 1}};
        ProfileField<6> f(r, 1, {g, "linear", ""}, Editor::PiecewiseLinear, defaults3, b);
        const float points[] = {0, 0, midpoint, .7f, 1, 1};
        uint8_t raw[25]; Writer w(raw, sizeof(raw)); w.put(uint8_t{2}); w.bytes(points, 24);
        CHECK(f.update(raw, sizeof(raw)) == (midpoint == .5f));
    });
}

static void booleansEnumsAndWriter() {
    for (const uint8_t code : {uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{127}, uint8_t{255}}) {
        test("Boolean wire value " + std::to_string(code), [=] {
            Registry r; Group g(r, 1, "g"); ConfigField<bool> f(r, 1, {g, "bool", ""}, false);
            CHECK(put(f, code) == (code <= 1));
            CHECK(f.get() == (code == 1));
        });
    }
    test("Boolean reset restores true default", [] {
        Registry r; Group g(r, 1, "g"); ConfigField<bool> f(r, 1, {g, "bool", ""}, true);
        CHECK(put(f, uint8_t{0})); f.reset(); CHECK(f.get());
    });
    test("Boolean metadata omits numerical bounds", [] {
        Registry r; Group g(r, 1, "g"); ConfigField<bool> f(r, 1, {g, "bool", ""}, true);
        uint8_t bytes[3] = {}; Writer w(bytes, sizeof(bytes)); f.metadata(w);
        CHECK(w.good() && w.size() == 1 && bytes[0] == 1 && f.type == Type::Boolean);
    });
    enum class Selection : uint8_t { First = 3, Second = 9 };
    const Choice choices[] = {{3, "first"}, {9, "second"}};
    for (const uint8_t code : {uint8_t{0}, uint8_t{3}, uint8_t{8}, uint8_t{9}, uint8_t{255}}) {
        test("enum membership for byte " + std::to_string(code), [=, &choices] {
            Registry r; Group g(r, 1, "g");
            EnumField<Selection, 2> f(r, 1, {g, "enum", ""}, Selection::First, choices);
            CHECK(put(f, code) == (code == 3 || code == 9));
            CHECK(f.get() == (code == 9 ? Selection::Second : Selection::First));
        });
    }
    test("enum reset restores default choice", [&] {
        Registry r; Group g(r, 1, "g"); EnumField<Selection, 2> f(r, 1, {g, "enum", ""}, Selection::First, choices);
        CHECK(put(f, uint8_t{9})); f.reset(); CHECK(f.get() == Selection::First);
    });
    for (const size_t capacity : {size_t{0}, size_t{1}, size_t{2}}) {
        test("writer guards capacity " + std::to_string(capacity), [=] {
            uint8_t raw[] = {99, 99, 99}; Writer w(raw, capacity); w.put(uint16_t{0x1234});
            CHECK(w.good() == (capacity >= 2)); CHECK(raw[capacity] == 99);
        });
    }
    test("writer overflow status stays false after later valid write", [] {
        uint8_t raw[2]{}; Writer w(raw, sizeof(raw)); w.put(uint32_t{1}); w.put(uint8_t{2});
        CHECK(!w.good());
    });
    test("writer text length counts UTF-8 bytes", [] {
        uint8_t raw[3]{}; Writer w(raw, sizeof(raw)); w.text("\xc3\xa9");
        CHECK(w.good() && w.size() == 3 && raw[0] == 2 && raw[1] == 0xc3 && raw[2] == 0xa9);
    });
    test("writer accepts 255-byte string", [] {
        uint8_t raw[256]{}; Writer w(raw, sizeof(raw)); w.text(std::string(255, 'x').c_str());
        CHECK(w.good() && w.size() == 256 && raw[0] == 255);
    });
    test("writer rejects 256-byte string", [] {
        uint8_t raw[258]{}; Writer w(raw, sizeof(raw)); w.text(std::string(256, 'x').c_str());
        CHECK(!w.good() && w.size() == 0);
    });
}

struct Fixture {
    Registry registry;
    Group parent{registry, 1, "parent"};
    Group child{registry, 2, "child", false, &parent};
    ConfigField<uint16_t> a{registry, 10, {parent, "a", "help"}, 5, 0, 100};
    ConfigField<uint16_t> b{registry, 20, {child, "b", "help"}, 6, 0, 100};
    Memory memory;
    Persistence persistence{registry, memory};
    Protocol protocol;
    explicit Fixture(bool (*validate)() = nullptr) : protocol(registry, persistence, validate) { persistence.load(); }
    void queue(Command op, const std::vector<uint8_t> &body = {}) {
        uint8_t packet[MaxRequestSize];
        Writer writer(packet, sizeof(packet));
        writer.put(RequestTag); writer.put(ProtocolVersion); writer.put(uint8_t{91}); writer.put(static_cast<uint8_t>(op));
        if (!body.empty()) writer.bytes(body.data(), body.size());
        writer.put(crc16(packet, writer.size()));
        CHECK(writer.good() && protocol.queue(packet, writer.size()));
    }
    Status command(Command op, std::vector<uint8_t> body = {}, bool disarmed = true) {
        queue(op, body);
        uint8_t response[MaxResponseSize];
        const size_t size = protocol.reply(response, disarmed);
        CHECK(size >= 8 && response[2] == 91 && response[3] == static_cast<uint8_t>(op));
        CHECK(crc16(response, size - 2) == read<uint16_t>(response + size - 2));
        return static_cast<Status>(response[4]);
    }
};

static void registryAndStorage() {
    test("registry discovery order and stable lookup", [] { Fixture f; CHECK(f.registry.fieldAt(0) == &f.a); CHECK(f.registry.find(20) == &f.b); CHECK(!f.registry.fieldAt(2)); });
    test("group parent and self membership", [] { Fixture f; CHECK(f.child.within(&f.parent)); CHECK(f.parent.within(&f.parent)); CHECK(!f.parent.within(&f.child)); });
    test("nested defaults preserve unrelated fields", [] { Fixture f; put(f.a, uint16_t{40}); put(f.b, uint16_t{50}); f.registry.defaults(&f.child); CHECK(f.a.get() == 40 && f.b.get() == 6); });
    test("duplicate IDs preserve original registration", [] { Fixture f; ConfigField<int> duplicate(f.registry, 10, {f.child, "duplicate", ""}, 0, 0, 1); CHECK(f.registry.fieldCount() == 2 && f.registry.find(10) == &f.a); });
    test("erased storage loads defaults", [] { Fixture f; CHECK(f.a.get() == 5 && f.b.get() == 6 && !f.persistence.dirty()); });
    test("save then load restores modified values", [] { Fixture f; put(f.a, uint16_t{42}); f.persistence.refreshDirty(); CHECK(f.persistence.save()); put(f.a, uint16_t{12}); f.persistence.load(); CHECK(f.a.get() == 42); });
    test("unchanged save causes no storage writes", [] { Fixture f; CHECK(f.persistence.save()); const auto writes = f.memory.writes; CHECK(f.persistence.save() && f.memory.writes == writes); });
    test("editing back to saved value clears dirty", [] { Fixture f; put(f.a, uint16_t{42}); f.persistence.refreshDirty(); CHECK(f.persistence.dirty()); f.a.reset(); f.persistence.refreshDirty(); CHECK(!f.persistence.dirty()); });
    test("TLV buffer overflow reports failure", [] { Fixture f; uint8_t raw[3]; CHECK(snapshot(f.registry, raw, sizeof(raw)) == 0); });
    for (size_t length = 1; length < 6; ++length) test("truncated TLV length " + std::to_string(length), [=] {
        Fixture f; const uint8_t record[] = {10, 0, static_cast<uint8_t>(Type::U16), 2, 42, 0};
        CHECK(!validTlvs(record, length)); applyTlvs(f.registry, record, length); CHECK(f.a.get() == 5);
    });
    for (const uint8_t type : {uint8_t(Type::I16), uint8_t(Type::F32), uint8_t{255}}) test("TLV incompatible type " + std::to_string(type), [=] {
        Fixture f; const uint8_t record[] = {10, 0, type, 2, 42, 0}; applyTlvs(f.registry, record, sizeof(record)); CHECK(f.a.get() == 5);
    });
    test("unknown TLV does not hide following known record", [] {
        Fixture f; const uint8_t records[] = {99, 0, 255, 1, 1, 10, 0, uint8_t(Type::U16), 2, 42, 0};
        applyTlvs(f.registry, records, sizeof(records)); CHECK(f.a.get() == 42);
    });
    test("migration preserves IDs across rename and reorder", [] {
        Fixture f; put(f.a, uint16_t{42}); f.persistence.refreshDirty(); CHECK(f.persistence.save());
        Registry r; Group g(r, 9, "renamed group");
        ConfigField<uint16_t> b(r, 20, {g, "renamed b", ""}, 1, 0, 100);
        ConfigField<uint8_t> added(r, 30, {g, "new", ""}, 7, 0, 10);
        ConfigField<uint16_t> a(r, 10, {g, "renamed a", ""}, 2, 0, 100);
        Persistence p(r, f.memory); p.load(); CHECK(a.get() == 42 && b.get() == 6 && added.get() == 7);
    });
    test("migration changed type uses new default", [] {
        Fixture f; put(f.a, uint16_t{42}); f.persistence.refreshDirty(); CHECK(f.persistence.save());
        Registry r; Group g(r, 1, "g"); ConfigField<int16_t> field(r, 10, {g, "a", ""}, -2, -10, 100);
        Persistence p(r, f.memory); p.load(); CHECK(field.get() == -2);
    });
    test("migration narrower range uses default", [] {
        Fixture f; put(f.a, uint16_t{42}); f.persistence.refreshDirty(); CHECK(f.persistence.save());
        Registry r; Group g(r, 1, "g"); ConfigField<uint16_t> field(r, 10, {g, "a", ""}, 3, 0, 10);
        Persistence p(r, f.memory); p.load(); CHECK(field.get() == 3);
    });
    test("seven profiles survive reload and a damaged slot is skipped", [] {
        Fixture f;
        uint8_t payload[Persistence::ProfileCapacity], buffer[Persistence::ProfileCapacity];
        uint8_t name[Persistence::ProfileNameSize] = "Rider", actual[Persistence::ProfileNameSize];
        const size_t size = snapshot(f.registry, payload, sizeof(payload));
        for (uint8_t slot = 0; slot < Persistence::ProfileCount; ++slot)
            CHECK(f.persistence.writeProfile(slot, name, payload, size));
        CHECK(!f.persistence.writeProfile(Persistence::ProfileCount, name, payload, size));
        Persistence rebooted(f.registry, f.memory);
        rebooted.load();
        for (uint8_t slot = 0; slot < Persistence::ProfileCount; ++slot) {
            CHECK(rebooted.readProfile(slot, actual, buffer) == size);
            CHECK(memcmp(name, actual, sizeof(name)) == 0);
            CHECK(memcmp(payload, buffer, size) == 0);
        }
        f.memory.data[Persistence::SlotSize + Persistence::ProfileHeaderSize] ^= 1;
        CHECK(!rebooted.readProfile(0, actual, buffer));
        CHECK(rebooted.readProfile(1, actual, buffer) == size);
    });
    Fixture saved; put(saved.a, uint16_t{42}); saved.persistence.refreshDirty(); CHECK(saved.persistence.save());
    for (size_t offset = 0; offset < 28; ++offset) test("CRC rejects damaged bank byte " + std::to_string(offset), [&, offset] {
        Fixture f; f.memory.data = saved.memory.data; f.memory.data[offset] ^= 1; f.persistence.load(); CHECK(f.a.get() == 5);
    });
    Fixture writeCount; writeCount.memory.data = saved.memory.data; writeCount.persistence.load();
    put(writeCount.a, uint16_t{65}); writeCount.persistence.refreshDirty();
    const auto before = writeCount.memory.writes; CHECK(writeCount.persistence.save());
    const auto required = writeCount.memory.writes - before;
    for (size_t cutoff = 0; cutoff <= required; ++cutoff) test("save power cut after " + std::to_string(cutoff) + " changed bytes", [&, cutoff] {
        Fixture f; f.memory.data = saved.memory.data; f.persistence.load();
        put(f.a, uint16_t{65}); f.persistence.refreshDirty(); f.memory.budget = static_cast<int>(cutoff);
        const bool success = f.persistence.save(); f.persistence.load();
        CHECK(f.a.get() == (success ? 65 : cutoff == 0 ? 42 : 5)); CHECK(success == (cutoff == required));
    });
}

static void protocolTests() {
    test("profile chunks require Disarm and a successful upload leaves active settings alone", [] {
        Fixture f;
        std::vector<uint8_t> begin(27);
        begin[1] = 12;
        begin[3] = 'R';
        const std::vector<uint8_t> chunk{0, 0, 10, 0, uint8_t(Type::U16), 2, 42, 0,
                                         20, 0, uint8_t(Type::U16), 2, 6, 0};
        CHECK(f.command(Command::ProfileList, {}, false) == Status::Ok);
        CHECK(f.command(Command::ProfileBegin, begin, false) == Status::NotDisarmed);
        CHECK(f.command(Command::ProfileBegin, begin) == Status::Ok);
        CHECK(f.command(Command::ProfileChunk, chunk, false) == Status::NotDisarmed);
        CHECK(f.command(Command::ProfileCommit) == Status::Invalid);
        CHECK(f.command(Command::ProfileBegin, begin) == Status::Ok);
        CHECK(f.command(Command::ProfileChunk, chunk) == Status::Ok);
        CHECK(f.command(Command::ProfileCommit) == Status::Ok);
        CHECK(f.a.get() == 5 && f.protocol.revision() == 0 && !f.persistence.dirty());
        CHECK(f.command(Command::ProfileRead, {0, 0, 0}, false) == Status::Ok);
    });
    test("move swaps adjacent profiles, requires Disarm, and selects into RAM", [] {
        Fixture f;
        uint8_t payload[Persistence::ProfileCapacity];
        uint8_t first[Persistence::ProfileNameSize] = "First", second[Persistence::ProfileNameSize] = "Second";
        CHECK(f.persistence.writeProfile(3, first, payload, snapshot(f.registry, payload, sizeof(payload))));
        CHECK(put(f.a, uint16_t{42}));
        CHECK(f.persistence.writeProfile(4, second, payload, snapshot(f.registry, payload, sizeof(payload))));
        f.persistence.load();
        CHECK(f.command(Command::ProfileMove, {4, 3}, false) == Status::NotDisarmed);
        CHECK(f.command(Command::ProfileMove, {4, 2}) == Status::Invalid);
        CHECK(f.command(Command::ProfileMove, {4, 3}) == Status::Ok);
        CHECK(f.a.get() == 5);
        CHECK(f.protocol.selectProfile(3) && f.a.get() == 42);
        CHECK(f.protocol.selectProfile(4) && f.a.get() == 5);
        CHECK(f.command(Command::ProfileMove, {4, 5}) == Status::Ok);
        CHECK(!f.protocol.selectProfile(4) && f.protocol.selectProfile(5));
    });
    test("selecting a profile from older firmware gives its missing fields their defaults", [] {
        Fixture f;
        // A profile saved before field b existed: only a's record.
        const uint8_t older[] = {10, 0, uint8_t(Type::U16), 2, 42, 0};
        uint8_t name[Persistence::ProfileNameSize] = "Older";
        CHECK(f.persistence.writeProfile(2, name, older, sizeof(older)));
        CHECK(put(f.b, uint16_t{77})); // left over from the previously active profile
        CHECK(f.protocol.selectProfile(2));
        CHECK(f.a.get() == 42 && f.b.get() == 6);
    });
    test("a rejected profile selection restores every field, including ones it lacked", [] {
        static bool accept = true;
        Fixture f([] { return accept; });
        const uint8_t older[] = {10, 0, uint8_t(Type::U16), 2, 42, 0};
        uint8_t name[Persistence::ProfileNameSize] = "Older";
        CHECK(f.persistence.writeProfile(2, name, older, sizeof(older)));
        CHECK(put(f.a, uint16_t{9}) && put(f.b, uint16_t{77}));
        accept = false;
        CHECK(!f.protocol.selectProfile(2));
        accept = true;
        CHECK(f.a.get() == 9 && f.b.get() == 77);
    });
    static_assert(static_cast<uint8_t>(Status::NotDisarmed) == 5, "Keep the dashboard status identifier stable");
    test("leaving DISARM before a queued write executes rejects the change", [] {
        Fixture f;
        bool disarmed = true;
        f.queue(Command::Set, {10, 0, 42, 0});
        CHECK(f.a.get() == 5 && f.protocol.pending());
        disarmed = false;
        uint8_t response[MaxResponseSize];
        CHECK(f.protocol.reply(response, disarmed) == 8);
        CHECK(response[4] == static_cast<uint8_t>(Status::NotDisarmed));
        CHECK(f.a.get() == 5 && f.protocol.revision() == 0 && !f.persistence.dirty());
    });
    test("entering DISARM before a queued write executes allows the change", [] {
        Fixture f;
        bool disarmed = false;
        f.queue(Command::Set, {10, 0, 42, 0});
        disarmed = true;
        uint8_t response[MaxResponseSize];
        CHECK(f.protocol.reply(response, disarmed) > 8);
        CHECK(response[4] == static_cast<uint8_t>(Status::Ok));
        CHECK(f.a.get() == 42 && f.protocol.revision() == 1 && f.persistence.dirty());
    });
    test("group default cross-validation failure restores the entire previous configuration", [] {
        Fixture f([] { return false; });
        CHECK(put(f.a, uint16_t{40}) && put(f.b, uint16_t{50}));
        CHECK(f.command(Command::Defaults, {0, 0}) == Status::Invalid);
        CHECK(f.a.get() == 40 && f.b.get() == 50 && f.protocol.revision() == 0);
    });
    const std::vector<std::pair<Command, std::vector<uint8_t>>> mutations = {
        {Command::Set, {10, 0, 42, 0}}, {Command::Save, {}}, {Command::Revert, {}}, {Command::Defaults, {0, 0}}
    };
    for (const auto &operation : mutations) {
        test("operation " + std::to_string(int(operation.first)) + " rejects changes outside DISARM", [&] {
            Fixture f; const auto old = f.memory.data;
            CHECK(f.command(operation.first, operation.second, false) == Status::NotDisarmed);
            CHECK(f.a.get() == 5 && f.memory.data == old && f.protocol.revision() == 0);
            CHECK(!f.persistence.dirty());
        });
        test("operation " + std::to_string(int(operation.first)) + " accepts changes in DISARM", [&] {
            Fixture f; CHECK(f.command(operation.first, operation.second, true) == Status::Ok);
        });
        test("operation " + std::to_string(int(operation.first)) + " preserves dirty RAM and EEPROM when armed", [&] {
            Fixture f;
            CHECK(f.command(Command::Set, {10, 0, 55, 0}) == Status::Ok);
            const auto old = f.memory.data;
            const auto revision = f.protocol.revision();
            CHECK(f.command(operation.first, operation.second, false) == Status::NotDisarmed);
            CHECK(f.a.get() == 55 && f.b.get() == 6 && f.memory.data == old);
            CHECK(f.protocol.revision() == revision && f.persistence.dirty());
        });
        test("operation " + std::to_string(int(operation.first)) + " recovers after switching to DISARM", [&] {
            Fixture f;
            CHECK(f.command(operation.first, operation.second, false) == Status::NotDisarmed);
            CHECK(f.command(operation.first, operation.second, true) == Status::Ok);
        });
    }
    const std::vector<std::pair<Command, std::vector<uint8_t>>> reads = {
        {Command::Hello, {}}, {Command::Group, {0, 0}}, {Command::Describe, {0, 0}}, {Command::Read, {10, 0}}
    };
    for (const auto &operation : reads) test("read operation " + std::to_string(int(operation.first)) + " outside DISARM", [&] {
        Fixture f; CHECK(f.command(operation.first, operation.second, false) == Status::Ok);
    });
    for (int op = 1; op <= 9; ++op) test("malformed request body for operation " + std::to_string(op), [=] {
        Fixture f; CHECK(f.command(static_cast<Command>(op), {0xff}) == Status::Malformed);
    });
    for (const auto op : {Command::Read, Command::Set, Command::Defaults, Command::Preview}) test("unknown ID for operation " + std::to_string(int(op)), [=] {
        Fixture f; auto body = std::vector<uint8_t>{99, 0}; if (op == Command::Set) body.insert(body.end(), {1, 0});
        CHECK(f.command(op, body) == Status::Unknown);
    });
    test("set rejects out-of-range value without revision", [] { Fixture f; CHECK(f.command(Command::Set, {10, 0, 101, 0}) == Status::Invalid); CHECK(f.a.get() == 5 && f.protocol.revision() == 0); });
    // RAM-only fields (FieldInfo::persistent = false), e.g. the bench-test trigger.
    struct RamOnly : Fixture {
        ConfigField<uint8_t> trigger{registry, 30, {parent, "trigger", "help", "", false, false}, 0, 0, 1};
        RamOnly() { persistence.load(); }
    };
    test("RAM-only field is left out of snapshots, saves and the dirty flag", [] {
        RamOnly f;
        uint8_t raw[64];
        const size_t withoutTrigger = snapshot(f.registry, raw, sizeof(raw));
        CHECK(withoutTrigger == 2 * 6); // only a and b
        CHECK(f.command(Command::Set, {30, 0, 1}) == Status::Ok && f.trigger.get() == 1);
        CHECK(!f.persistence.dirty());
        CHECK(snapshot(f.registry, raw, sizeof(raw)) == withoutTrigger);
        CHECK(f.command(Command::Save) == Status::Ok);
        f.persistence.load();
        CHECK(f.trigger.get() == 0);
    });
    test("RAM-only field ignores a stored record and resets on revert", [] {
        RamOnly f;
        const uint8_t record[] = {30, 0, static_cast<uint8_t>(Type::U8), 1, 1};
        applyTlvs(f.registry, record, sizeof(record));
        CHECK(f.trigger.get() == 0);
        CHECK(f.command(Command::Set, {30, 0, 1}) == Status::Ok);
        CHECK(f.command(Command::Revert) == Status::Ok && f.trigger.get() == 0);
    });
    test("profile upload drops a RAM-only record and still needs every persistent field", [] {
        RamOnly f;
        const std::vector<uint8_t> all{10, 0, uint8_t(Type::U16), 2, 42, 0,
                                       30, 0, uint8_t(Type::U8), 1, 1,
                                       20, 0, uint8_t(Type::U16), 2, 6, 0};
        std::vector<uint8_t> begin(27);
        begin[1] = static_cast<uint8_t>(all.size());
        begin[3] = 'R';
        std::vector<uint8_t> chunk{0, 0};
        chunk.insert(chunk.end(), all.begin(), all.end());
        CHECK(f.command(Command::ProfileBegin, begin) == Status::Ok);
        CHECK(f.command(Command::ProfileChunk, chunk) == Status::Ok);
        CHECK(f.command(Command::ProfileCommit) == Status::Ok);
        CHECK(f.trigger.get() == 0 && f.a.get() == 5); // validating it changed nothing in RAM
        uint8_t name[Persistence::ProfileNameSize], payload[Persistence::ProfileCapacity];
        const size_t length = f.persistence.readProfile(0, name, payload);
        CHECK(length == 12);
        for (size_t offset = 0; offset < length; offset += 4 + payload[offset + 3])
            CHECK(read<uint16_t>(payload + offset) != 30);
        CHECK(f.protocol.selectProfile(0) && f.a.get() == 42 && f.trigger.get() == 0);
        // The RAM-only record can't stand in for a persistent one.
        const std::vector<uint8_t> missing{10, 0, uint8_t(Type::U16), 2, 42, 0, 30, 0, uint8_t(Type::U8), 1, 1};
        begin[1] = static_cast<uint8_t>(missing.size());
        std::vector<uint8_t> chunk2{0, 0};
        chunk2.insert(chunk2.end(), missing.begin(), missing.end());
        CHECK(f.command(Command::ProfileBegin, begin) == Status::Ok);
        CHECK(f.command(Command::ProfileChunk, chunk2) == Status::Ok);
        CHECK(f.command(Command::ProfileCommit) == Status::Invalid);
    });
    test("set rejects wrong value width", [] { Fixture f; CHECK(f.command(Command::Set, {10, 0, 1}) == Status::Malformed); });
    test("unknown operation reports Unknown", [] { Fixture f; CHECK(f.command(static_cast<Command>(255)) == Status::Unknown); });
    test("save and revert restore acknowledged value", [] { Fixture f; CHECK(f.command(Command::Set, {10, 0, 42, 0}) == Status::Ok); CHECK(f.command(Command::Save) == Status::Ok); CHECK(f.command(Command::Set, {10, 0, 12, 0}) == Status::Ok); CHECK(f.command(Command::Revert) == Status::Ok && f.a.get() == 42); });
    test("idle protocol produces no reply", [] { Fixture f; uint8_t response[MaxResponseSize]; CHECK(f.protocol.reply(response, false) == 0); });
    for (size_t length = 0; length < 6; ++length) test("short packet length " + std::to_string(length), [=] {
        Fixture f; const uint8_t packet[] = {RequestTag, 1, 0, 1, 0, 0}; f.protocol.queue(packet, length); CHECK(!f.protocol.pending());
    });
}

int main() {
    scalars<float>("float", Type::F32); scalars<double>("double", Type::F64);
    scalars<int8_t>("int8", Type::I8); scalars<uint8_t>("uint8", Type::U8);
    scalars<int16_t>("int16", Type::I16); scalars<uint16_t>("uint16", Type::U16);
    scalars<int32_t>("int32", Type::I32); scalars<uint32_t>("uint32", Type::U32);
    scalars<int64_t>("int64", Type::I64); scalars<uint64_t>("uint64", Type::U64);
    scalars<unsigned long>("unsigned long", scalarType<unsigned long>());
    booleansEnumsAndWriter(); profiles(); registryAndStorage(); protocolTests();
    std::cout << "TOTAL " << passed + failed << " / PASS " << passed << " / FAIL " << failed << '\n';
    return failed ? 1 : 0;
}
