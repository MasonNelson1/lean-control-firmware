#include "telemetry/Telemetry.h"
#include <Arduino.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

TestSerial Serial, Serial1;

static std::vector<std::string> columns(const char *row)
{
    std::vector<std::string> result;
    std::string text(row);
    size_t from = 0, comma;
    while ((comma = text.find(',', from)) != std::string::npos)
    {
        result.push_back(text.substr(from, comma - from));
        from = comma + 1;
    }
    result.push_back(text.substr(from));
    return result;
}

int main()
{
    telemetry::Frame f;
    f.timeMs = 1000;
    f.mode = common::LEAN;
    f.lean = {100, 200, 300, 400, 0.25f, -0.1f, false};
    f.userWeight = 370;
    f.commands = {10, 20, 9, 19};
    f.left = {8, 500, true};
    f.right = {18, 499, true};
    char row[256];
    auto length = telemetry::formatCSV(f, row, sizeof(row));
    assert(length == std::string(row).size());
    auto c = columns(row);
    assert(c.size() == 19 && columns(telemetry::CsvHeader).size() == 19);
    assert(c[0] == "1000" && c[5] == "1000" && c[6] == "370");
    assert(c[7] == "0.2500" && c[8] == "-0.1000");
    assert(c[9] == "10.0000" && c[12] == "19.0000");
    assert(c[13] == "0" && c[14] == "8.0000" && std::isnan(std::stof(c[15])));
    assert(c[18] == "LEAN\n");
    assert(telemetry::formatCSV(f, row, 8) == 0);
    assert(telemetry::formatCSV(f, nullptr, 0) == 0);
    for (auto mode : {common::DISARM, common::ASSIST})
    {
        f.mode = mode;
        f.boostCurrent = {1.5f, -2.5f};
        telemetry::formatCSV(f, row, sizeof(row));
        c = columns(row);
        for (int i = 9; i <= 12; ++i) assert(std::isnan(std::stof(c[i])));
        assert(c[16] == "1.5000" && c[17] == "-2.5000");
        assert(c[13] == "1");
    }
    f.mode = common::REMOTE;
    f.remoteStopped = false;
    telemetry::formatCSV(f, row, sizeof(row));
    assert(columns(row)[13] == "0");
    assert(columns(row)[18] == "REMOTE\n");
    f.remoteStopped = true;
    telemetry::formatCSV(f, row, sizeof(row));
    assert(columns(row)[13] == "1");
    telemetry::MotorFeedback motor{3, UINT32_MAX - 100, true};
    assert(motor.actualRPM(399) == 3);
    assert(std::isnan(motor.actualRPM(400)));
    motor.received = false;
    assert(std::isnan(motor.actualRPM(0)));

    telemetry::Telemetry output;
    assert(!output.due(49) && output.due(50));
    assert(!output.due(99) && output.due(100));
    assert(output.due(UINT32_MAX - 10));
    assert(!output.due(38) && output.due(39));
    output.publish(f);
    const std::string csv(row);
    assert(Serial.output == std::string(telemetry::CsvHeader) + csv);
    assert(Serial1.output == csv); // Bluetooth stays headerless
    output.publish(f);
    assert(Serial.output == std::string(telemetry::CsvHeader) + csv + csv);
    Serial.output.clear();
    Serial1.output.clear();
    Serial1.capacity = 0;
    output.publish(f);
    assert(Serial.output == csv && Serial1.output.empty());
    telemetry::Telemetry::logf("Test %d\n", 42);
    assert(Serial1.output.empty());
    Serial1.capacity = 512;
    Serial.output.clear();
    telemetry::Telemetry::logf("Test %d\n", 42);
    assert(Serial.output == "Test 42\n" && Serial1.output == Serial.output);
    Serial.output.clear();
    Serial1.output.clear();
    telemetry::Telemetry::logf("%s", std::string(300, 'x').c_str());
    assert(Serial.output.empty() && Serial1.output.empty());
    puts("Telemetry CSV, timing, and transport regression tests passed");
}
