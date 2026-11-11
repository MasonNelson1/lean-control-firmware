// g++ -std=c++17 -Wall -Wextra -Werror -I tests/native/config_stubs -I src tests/native/drive_settings_test.cpp src/config/DriveSettings.cpp src/config/Registry.cpp src/config/Persistence.cpp src/config/Protocol.cpp -o .pio/drive_settings_test.exe
#include "config/DriveSettings.h"
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace config;
static int passed = 0, failed = 0;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)
static void test(const std::string &name, const std::function<void()> &body) {
    try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
    catch (const std::exception &error) { ++failed; std::cout << "FAIL " << name << " -- " << error.what() << '\n'; }
}
static Status request(Command command, uint16_t id, const uint8_t *value = nullptr, size_t size = 0) {
    uint8_t data[MaxRequestSize], response[MaxResponseSize];
    Writer out(data, sizeof(data));
    out.put(RequestTag); out.put(ProtocolVersion); out.put(uint8_t{1}); out.put(static_cast<uint8_t>(command));
    out.put(id);
    if (size) out.bytes(value, size);
    out.put(crc16(data, out.size()));
    CHECK(settings::protocol.queue(data, out.size()));
    CHECK(settings::protocol.reply(response, 0, 0) >= 8);
    return static_cast<Status>(response[4]);
}
int main() {
    settings::persistence.load();
    for (const auto *field = settings::registry().fields(); field; field = field->next()) {
        const std::string name = std::to_string(field->id) + " " + field->info.name;
        test("actual declaration round trip / " + name, [&] {
            uint8_t raw[MaxValueSize]; Writer out(raw, sizeof(raw)); field->value(out);
            CHECK(out.good() && out.size() == field->width());
            CHECK(settings::registry().find(field->id)->update(raw, out.size()));
        });
        test("actual declaration fits discovery packet / " + name, [&] {
            uint16_t index = 0;
            while (settings::registry().fieldAt(index) != field) ++index;
            CHECK(request(Command::Describe, index) == Status::Ok);
            CHECK(field->info.name[0] && field->info.description[0]);
        });
    }
    for (const auto *group = settings::registry().groups(); group; group = group->next()) {
        test("actual group resolves parent / " + std::string(group->name), [&] {
            CHECK(group->id && group->name[0]);
            if (group->parent) CHECK(settings::registry().findGroup(group->parent->id) == group->parent);
        });
    }
    test("actual field IDs are unique and nonzero", [] {
        std::set<uint16_t> ids;
        for (auto *f = settings::registry().fields(); f; f = f->next()) CHECK(f->id && ids.insert(f->id).second);
    });
    test("every exported declaration is discoverable under its own ID", [] {
        const std::vector<const Field *> exported = {
            &settings::defaultMode, &settings::deadband,
            &settings::frontGain, &settings::rearGain, &settings::leftGain, &settings::rightGain,
            &settings::frontWeight, &settings::rearWeight, &settings::leftWeight, &settings::rightWeight,
            &settings::leanResponse, &settings::controllerTimeout, &settings::buttonDebounce,
            &settings::controlPeriod, &settings::calibrationSamples, &settings::defaultFloor,
            &settings::leftVesc, &settings::rightVesc, &settings::wheelCircumference,
            &settings::normalPid.lowTau, &settings::normalPid.highTau, &settings::normalPid.ki,
            &settings::normalPid.kd, &settings::normalPid.filter, &settings::normalPid.maxJerk,
            &settings::transitionPid.lowTau, &settings::transitionPid.highTau, &settings::transitionPid.ki,
            &settings::transitionPid.kd, &settings::transitionPid.filter, &settings::transitionPid.maxJerk
        };
        CHECK(exported.size() == settings::registry().fieldCount());
        for (const auto *field : exported) CHECK(settings::registry().find(field->id) == field);
    });
    test("actual live CAN ID conflict is rejected and rolled back", [] {
        const uint8_t previous = settings::leftVesc.get(), collision = settings::rightVesc.get();
        CHECK(request(Command::Set, settings::leftVesc.id, &collision, 1) == Status::Invalid);
        CHECK(settings::leftVesc.get() == previous);
    });
    test("actual PID preview bindings reference registered fields", [] {
        size_t count = 0;
        for (const auto *g = settings::registry().groups(); g; g = g->next()) {
            if (!g->preview) continue;
            ++count;
            CHECK(g->preview->type == 1 && g->preview->fieldCount == 8 && g->preview->constantCount == 2);
            for (uint8_t i = 0; i < g->preview->fieldCount; ++i) CHECK(settings::registry().find(g->preview->fieldIds[i]));
        }
        CHECK(count == 2);
    });
    test("actual runtime edits mark persistence dirty", [] {
        const float value = .10f;
        CHECK(request(Command::Set, settings::deadband.id, reinterpret_cast<const uint8_t *>(&value), sizeof(value)) == Status::Ok);
        CHECK(settings::persistence.dirty());
        settings::persistence.load();
    });
    test("actual profile accepts an amplifying knee", [] {
        uint8_t raw[9]; Writer out(raw, sizeof(raw)); out.put(uint8_t{1}); out.put(.25f); out.put(.75f);
        CHECK(request(Command::Set, settings::leanResponse.id, raw, sizeof(raw)) == Status::Ok);
        settings::persistence.load();
    });
    test("actual settings fit EEPROM and survive reload", [] {
        CHECK(settings::persistence.save());
        settings::persistence.load();
        CHECK(!settings::persistence.dirty());
    });
    test("loading conflicting saved CAN IDs repairs only the conflicting pair", [] {
        const uint8_t collision = settings::rightVesc.get();
        const float deadband = .2f;
        CHECK(settings::leftVesc.update(&collision, sizeof(collision)));
        CHECK(settings::deadband.update(reinterpret_cast<const uint8_t *>(&deadband), sizeof(deadband)));
        settings::persistence.refreshDirty();
        CHECK(settings::persistence.save());
        settings::persistence.load();
        CHECK(settings::leftVesc.get() != settings::rightVesc.get());
        CHECK(settings::deadband.get() == deadband);
    });
    std::cout << "TOTAL " << passed + failed << " / PASS " << passed << " / FAIL " << failed << '\n';
    return failed ? 1 : 0;
}
