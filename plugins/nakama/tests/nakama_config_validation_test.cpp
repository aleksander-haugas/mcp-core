#include "../src/nakama_plugin.cpp"

#include <cstdio>
#include <string>
#include <vector>

struct TestCase {
    const char* name;
    std::string input;
    bool expected;
};

int main()
{
    const std::string valid =
        "{"
        "\"server_url\":\"wss://192.168.100.3:7350\","
        "\"server_key\":\"test-server-key\","
        "\"device_id\":\"test-device-id\","
        "\"connect_timeout_ms\":10000,"
        "\"verify_tls\":true"
        "}";

    std::string embedded_nul = valid;
    embedded_nul.insert(
        embedded_nul.find("test-device-id") + 4,
        1,
        '\0'
    );

    const std::string duplicate_key =
        valid.substr(0, valid.size() - 1) +
        R"(,"server_url":"wss://other"})";

    const std::string escaped_duplicate_key =
        valid.substr(0, valid.size() - 1) +
        R"(,"server\u005furl":"wss://other"})";

    std::string punctuation_value = valid;
    const std::string device_marker = "\"test-device-id\"";
    const size_t device_position =
        punctuation_value.find(device_marker);

    if (device_position == std::string::npos) {
        std::fprintf(stderr, "Could not locate device test value\n");
        return 2;
    }

    punctuation_value.replace(
        device_position,
        device_marker.size(),
        R"("device } { \"quoted\" \\ path")"
    );

    const auto config_with_url = [&valid](const std::string& url) {
        std::string config = valid;
        const std::string original = "wss://192.168.100.3:7350";

        const size_t position = config.find(original);
        if (position == std::string::npos) {
            return std::string();
        }

        config.replace(position, original.size(), url);
        return config;
    };

    const auto config_with_transport =
        [&config_with_url](const std::string& url, bool verify_tls) {
            std::string config = config_with_url(url);
            const std::string original = "\"verify_tls\":true";
            const std::string replacement = verify_tls
                ? "\"verify_tls\":true"
                : "\"verify_tls\":false";

            const size_t position = config.find(original);
            if (position == std::string::npos) {
                return std::string();
            }

            config.replace(position, original.size(), replacement);
            return config;
        };

    const std::vector<TestCase> tests = {
        {"Valid configuration", valid, true},
        {"Duplicate server_url key", duplicate_key, false},
        {"Escaped duplicate server_url key", escaped_duplicate_key, false},
        {"Braces and quotes inside string values", punctuation_value, true},
        {"Trailing whitespace", valid + " \r\n\t", true},
        {"Truncated JSON", valid.substr(0, valid.size() - 1), false},
        {"Trailing garbage", valid + "x", false},
        {"Extra property",
         valid.substr(0, valid.size() - 1) +
         ",\"extra\":true}", false},
        {"Missing property",
         "{\"server_url\":\"wss://host\","
         "\"server_key\":\"key\","
         "\"device_id\":\"device\","
         "\"verify_tls\":true}", false},
        {"Wrong timeout type",
         "{\"server_url\":\"wss://host\","
         "\"server_key\":\"key\","
         "\"device_id\":\"device\","
         "\"connect_timeout_ms\":\"10000\","
         "\"verify_tls\":true}", false},
        {"Timeout below minimum",
         "{\"server_url\":\"wss://host\","
         "\"server_key\":\"key\","
         "\"device_id\":\"device\","
         "\"connect_timeout_ms\":999,"
         "\"verify_tls\":true}", false},
        {"TLS verification disabled",
         "{\"server_url\":\"wss://host\","
         "\"server_key\":\"key\","
         "\"device_id\":\"device\","
         "\"connect_timeout_ms\":10000,"
         "\"verify_tls\":false}", false},
        {"WSS hostname without explicit port",
         config_with_url("wss://example.com"), true},
        {"WSS hostname with root path",
         config_with_url("wss://example.com/"), true},
        {"WSS empty authority",
         config_with_url("wss:///path"), false},
        {"WSS empty port",
         config_with_url("wss://example.com:"), false},
        {"WSS port zero",
         config_with_url("wss://example.com:0"), false},
        {"WSS port above maximum",
         config_with_url("wss://example.com:65536"), false},
        {"WSS extremely long port",
         config_with_url(
             "wss://example.com:" + std::string(1000, '9')
         ), false},
        {"WSS URL with credentials",
         config_with_url("wss://user:pass@example.com"), false},
        {"WSS URL with path",
         config_with_url("wss://example.com/nakama"), false},
        {"WSS URL with query",
         config_with_url("wss://example.com?x=1"), false},
        {"WSS URL with fragment",
         config_with_url("wss://example.com#fragment"), false},
        {"WSS with TLS verification enabled",
         config_with_transport("wss://example.com", true), true},
        {"WSS with TLS verification disabled",
         config_with_transport("wss://example.com", false), false},
        {"WS local transport without TLS",
         config_with_transport("ws://192.168.100.3:7350", false), true},
        {"WS transport with TLS verification enabled",
         config_with_transport("ws://192.168.100.3:7350", true), false},
        {"Non-WSS URL",
         "{\"server_url\":\"ws://host\","
         "\"server_key\":\"key\","
         "\"device_id\":\"device\","
         "\"connect_timeout_ms\":10000,"
         "\"verify_tls\":true}", false},
        {"Empty server key",
         "{\"server_url\":\"wss://host\","
         "\"server_key\":\"\","
         "\"device_id\":\"device\","
         "\"connect_timeout_ms\":10000,"
         "\"verify_tls\":true}", false},
        {"Embedded NUL", embedded_nul, false},
        {"Empty input", "", false},
        {"JSON null", "null", false},
        {"Array instead of object", "[]", false}
    };

    unsigned extraction_failures = 0;

    NakamaConfig extracted;
    const bool extraction_ok = parse_config_json(valid, extracted);

    if (!extraction_ok ||
        extracted.server_url != "wss://192.168.100.3:7350" ||
        extracted.server_key != "test-server-key" ||
        extracted.device_id != "test-device-id" ||
        extracted.connect_timeout_ms != 10000 ||
        !extracted.verify_tls) {
        ++extraction_failures;
    std::printf("FAIL: Configuration extraction\n");
        } else {
            std::printf("PASS: Configuration extraction\n");
        }

        NakamaConfig local_config;
       const std::string local_json =
           config_with_transport("ws://192.168.100.3:7350", false);

       if (!parse_config_json(local_json, local_config) ||
           local_config.server_url != "ws://192.168.100.3:7350" ||
           local_config.verify_tls) {
           ++extraction_failures;
           std::printf("FAIL: Local WS configuration extraction\n");
       } else {
           std::printf("PASS: Local WS configuration extraction\n");
       }

       std::string custom_timeout = valid;
        const size_t timeout_position = custom_timeout.find("10000");

        if (timeout_position == std::string::npos) {
            ++extraction_failures;
            std::printf("FAIL: Custom timeout test setup\n");
        } else {
            custom_timeout.replace(timeout_position, 5, "25000");

            NakamaConfig custom;
            const bool custom_ok = parse_config_json(custom_timeout, custom);

            if (!custom_ok || custom.connect_timeout_ms != 25000) {
                ++extraction_failures;
                std::printf("FAIL: Custom timeout extraction\n");
            } else {
                std::printf("PASS: Custom timeout extraction\n");
            }
        }

        NakamaConfig preserved;
        preserved.server_url = "preserved-url";
        preserved.server_key = "preserved-key";
        preserved.device_id = "preserved-device";
        preserved.connect_timeout_ms = 12345;
        preserved.verify_tls = false;

        const bool invalid_accepted = parse_config_json(
            valid + "invalid",
            preserved
        );

        if (invalid_accepted ||
            preserved.server_url != "preserved-url" ||
            preserved.server_key != "preserved-key" ||
            preserved.device_id != "preserved-device" ||
            preserved.connect_timeout_ms != 12345 ||
            preserved.verify_tls) {
            ++extraction_failures;
        std::printf("FAIL: Invalid input preserves output\n");
            } else {
                std::printf("PASS: Invalid input preserves output\n");
            }

        unsigned failures = extraction_failures;

    for (const auto& test : tests) {
        const bool actual = validate_config_json(test.input);
        const bool passed = actual == test.expected;

        std::printf(
            "%s: %s\n",
            passed ? "PASS" : "FAIL",
            test.name
        );

        if (!passed) {
            ++failures;
        }
    }

    std::printf("\nFailures: %u\n", failures);
    return failures == 0 ? 0 : 1;
}
