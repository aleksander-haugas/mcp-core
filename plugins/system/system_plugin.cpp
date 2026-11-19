
#include "mcp/plugin_abi.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <new>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>

namespace {

constexpr const char* PLUGIN_ID = "system";

struct SystemPluginContext {
    const mcp_host_api* host;
};

static const char EMPTY_OBJECT_SCHEMA[] =
    "{\"type\":\"object\",\"properties\":{},"
    "\"additionalProperties\":false}";

static const char* const SYSTEM_CAPABILITIES[] = {
    "system.read"
};

static const mcp_tool_definition TOOLS[] = {
    {
        "system_info",
        "Get operating system, kernel and host information",
        EMPTY_OBJECT_SCHEMA,
        SYSTEM_CAPABILITIES,
        sizeof(SYSTEM_CAPABILITIES) /
        sizeof(SYSTEM_CAPABILITIES[0])
    },
    {
        "system_uptime",
        "Get system uptime and boot timestamp",
        EMPTY_OBJECT_SCHEMA,
        SYSTEM_CAPABILITIES,
        sizeof(SYSTEM_CAPABILITIES) /
        sizeof(SYSTEM_CAPABILITIES[0])
    },
    {
        "system_hostname",
        "Get the system hostname",
        EMPTY_OBJECT_SCHEMA,
        SYSTEM_CAPABILITIES,
        sizeof(SYSTEM_CAPABILITIES) /
        sizeof(SYSTEM_CAPABILITIES[0])
    },
    {
        "system_cpu_info",
        "Get processor model, architecture and logical CPU count",
        EMPTY_OBJECT_SCHEMA,
        SYSTEM_CAPABILITIES,
        sizeof(SYSTEM_CAPABILITIES) /
        sizeof(SYSTEM_CAPABILITIES[0])
    },
    {
        "system_memory_info",
        "Get physical memory and virtual memory page statistics",
        EMPTY_OBJECT_SCHEMA,
        SYSTEM_CAPABILITIES,
        sizeof(SYSTEM_CAPABILITIES) /
        sizeof(SYSTEM_CAPABILITIES[0])
    },
    {
        "system_load_average",
        "Get the 1, 5 and 15 minute system load averages",
        EMPTY_OBJECT_SCHEMA,
        SYSTEM_CAPABILITIES,
        sizeof(SYSTEM_CAPABILITIES) /
        sizeof(SYSTEM_CAPABILITIES[0])
    }
};

void log_message(
    SystemPluginContext* context,
    mcp_log_level level,
    const char* message
)
{
    if (context == nullptr ||
        context->host == nullptr ||
        context->host->log == nullptr) {
        return;
    }

    context->host->log(
        context->host->host_context,
        level,
        PLUGIN_ID,
        message
    );
}

std::string json_escape(const std::string& value)
{
    std::ostringstream output;

    for (unsigned char ch : value) {
        switch (ch) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (ch < 0x20) {
                output << "\\u"
                       << std::hex
                       << std::setw(4)
                       << std::setfill('0')
                       << static_cast<unsigned int>(ch)
                       << std::dec;
            } else {
                output << static_cast<char>(ch);
            }
            break;
        }
    }

    return output.str();
}

template <typename T>
bool read_sysctl(
    const char* name,
    T& value
)
{
    size_t size = sizeof(T);

    return sysctlbyname(
        name,
        &value,
        &size,
        nullptr,
        0
    ) == 0 && size == sizeof(T);
}

bool read_sysctl_string(
    const char* name,
    std::string& value
)
{
    size_t size = 0;

    if (sysctlbyname(
            name,
            nullptr,
            &size,
            nullptr,
            0
        ) != 0 || size == 0 || size > 65536) {
        return false;
    }

    std::vector<char> buffer(size + 1, '\0');

    if (sysctlbyname(
            name,
            buffer.data(),
            &size,
            nullptr,
            0
        ) != 0) {
        return false;
    }

    value.assign(buffer.data());
    return true;
}

bool valid_empty_arguments(const char* arguments_json)
{
    if (arguments_json == nullptr) {
        return false;
    }

    const char* p = arguments_json;

    while (*p == ' ' || *p == '\t' ||
        *p == '\r' || *p == '\n') {
        ++p;
        }

        if (*p != '{') {
            return false;
        }

        ++p;

    while (*p == ' ' || *p == '\t' ||
        *p == '\r' || *p == '\n') {
        ++p;
        }

        if (*p != '}') {
            return false;
        }

        ++p;

    while (*p == ' ' || *p == '\t' ||
        *p == '\r' || *p == '\n') {
        ++p;
        }

        return *p == '\0';
}

bool make_system_info(std::string& result)
{
    struct utsname info {};

    if (uname(&info) != 0) {
        return false;
    }

    char hostname[256] = {};

    if (gethostname(hostname, sizeof(hostname) - 1) != 0) {
        return false;
    }

    std::ostringstream json;

    json << "{"
         << "\"os_name\":\"" << json_escape(info.sysname) << "\","
         << "\"hostname\":\"" << json_escape(hostname) << "\","
         << "\"kernel_release\":\"" << json_escape(info.release) << "\","
         << "\"kernel_version\":\"" << json_escape(info.version) << "\","
         << "\"machine\":\"" << json_escape(info.machine) << "\","
         << "\"platform\":\"FreeBSD\""
         << "}";

    result = json.str();
    return true;
}

bool make_hostname(std::string& result)
{
    char hostname[256] = {};

    if (gethostname(hostname, sizeof(hostname) - 1) != 0) {
        return false;
    }

    result = "{\"hostname\":\"";
    result += json_escape(hostname);
    result += "\"}";

    return true;
}

bool make_uptime(std::string& result)
{
    struct timeval boot_time {};
    struct timeval current_time {};

    size_t size = sizeof(boot_time);

    if (sysctlbyname(
            "kern.boottime",
            &boot_time,
            &size,
            nullptr,
            0
        ) != 0 || size != sizeof(boot_time)) {
        return false;
    }

    if (gettimeofday(&current_time, nullptr) != 0) {
        return false;
    }

    const int64_t boot_seconds =
        static_cast<int64_t>(boot_time.tv_sec);

    const int64_t current_seconds =
        static_cast<int64_t>(current_time.tv_sec);

    if (current_seconds < boot_seconds) {
        return false;
    }

    const uint64_t uptime =
        static_cast<uint64_t>(current_seconds - boot_seconds);

    std::ostringstream json;

    json << "{"
         << "\"uptime_seconds\":" << uptime << ","
         << "\"boot_timestamp\":" << boot_seconds
         << "}";

    result = json.str();
    return true;
}

bool make_cpu_info(std::string& result)
{
    std::string model;
    std::string architecture;
    std::string machine;

    int cpu_count = 0;

    if (!read_sysctl_string("hw.model", model) ||
        !read_sysctl_string("hw.machine_arch", architecture) ||
        !read_sysctl_string("hw.machine", machine) ||
        !read_sysctl("hw.ncpu", cpu_count)) {
        return false;
    }

    std::ostringstream json;

    json << "{"
         << "\"model\":\"" << json_escape(model) << "\","
         << "\"architecture\":\"" << json_escape(architecture) << "\","
         << "\"machine\":\"" << json_escape(machine) << "\","
         << "\"logical_processors\":" << cpu_count
         << "}";

    result = json.str();
    return true;
}

bool make_memory_info(std::string& result)
{
    uint64_t physical_memory = 0;
    int page_size = 0;

    uint32_t total_pages = 0;
    uint32_t free_pages = 0;
    uint32_t inactive_pages = 0;
    uint32_t cache_pages = 0;
    uint32_t wired_pages = 0;

    if (!read_sysctl("hw.physmem", physical_memory) ||
        !read_sysctl("hw.pagesize", page_size) ||
        !read_sysctl("vm.stats.vm.v_page_count", total_pages) ||
        !read_sysctl("vm.stats.vm.v_free_count", free_pages) ||
        !read_sysctl("vm.stats.vm.v_inactive_count", inactive_pages) ||
        !read_sysctl("vm.stats.vm.v_wire_count", wired_pages)) {
        return false;
    }

    // Cache pages are not available on every kernel configuration.
    // Treat this counter as optional.
    (void)read_sysctl(
        "vm.stats.vm.v_cache_count",
        cache_pages
    );

    if (page_size <= 0) {
        return false;
    }

    const uint64_t page_bytes =
        static_cast<uint64_t>(page_size);

    const uint64_t free_bytes =
        static_cast<uint64_t>(free_pages) * page_bytes;

    const uint64_t inactive_bytes =
        static_cast<uint64_t>(inactive_pages) * page_bytes;

    const uint64_t cache_bytes =
        static_cast<uint64_t>(cache_pages) * page_bytes;

    const uint64_t available_bytes =
        free_bytes + inactive_bytes + cache_bytes;

    const uint64_t total_page_bytes =
        static_cast<uint64_t>(total_pages) * page_bytes;

    std::ostringstream json;

    json << "{"
         << "\"physical_memory_bytes\":" << physical_memory << ","
         << "\"page_size_bytes\":" << page_size << ","
         << "\"total_pages\":" << total_pages << ","
         << "\"free_pages\":" << free_pages << ","
         << "\"inactive_pages\":" << inactive_pages << ","
         << "\"cache_pages\":" << cache_pages << ","
         << "\"wired_pages\":" << wired_pages << ","
         << "\"free_bytes\":" << free_bytes << ","
         << "\"inactive_bytes\":" << inactive_bytes << ","
         << "\"cache_bytes\":" << cache_bytes << ","
         << "\"estimated_available_bytes\":" << available_bytes << ","
         << "\"managed_memory_bytes\":" << total_page_bytes
         << "}";

    result = json.str();
    return true;
}

bool make_load_average(std::string& result)
{
    double values[3] = {};

    if (getloadavg(values, 3) != 3) {
        return false;
    }

    std::ostringstream json;

    json << std::setprecision(6)
         << "{"
         << "\"1_minute\":" << values[0] << ","
         << "\"5_minutes\":" << values[1] << ","
         << "\"15_minutes\":" << values[2]
         << "}";

    result = json.str();
    return true;
}

mcp_plugin_status execute(
    const char* tool_name,
    std::string& result
)
{
    if (std::strcmp(tool_name, "system_info") == 0) {
        return make_system_info(result)
        ? MCP_PLUGIN_OK : MCP_PLUGIN_ERROR;
    }

    if (std::strcmp(tool_name, "system_hostname") == 0) {
        return make_hostname(result)
        ? MCP_PLUGIN_OK : MCP_PLUGIN_ERROR;
    }

    if (std::strcmp(tool_name, "system_uptime") == 0) {
        return make_uptime(result)
        ? MCP_PLUGIN_OK : MCP_PLUGIN_ERROR;
    }

    if (std::strcmp(tool_name, "system_cpu_info") == 0) {
        return make_cpu_info(result)
        ? MCP_PLUGIN_OK : MCP_PLUGIN_ERROR;
    }

    if (std::strcmp(tool_name, "system_memory_info") == 0) {
        return make_memory_info(result)
        ? MCP_PLUGIN_OK : MCP_PLUGIN_ERROR;
    }

    if (std::strcmp(tool_name, "system_load_average") == 0) {
        return make_load_average(result)
        ? MCP_PLUGIN_OK : MCP_PLUGIN_ERROR;
    }

    return MCP_PLUGIN_NOT_FOUND;
}

mcp_plugin_status initialize(
    const mcp_host_api* host,
    const char* configuration_json,
    void** plugin_context
)
{
    (void)configuration_json;

    if (host == nullptr ||
        plugin_context == nullptr ||
        host->abi_version != MCP_PLUGIN_ABI_VERSION ||
        host->struct_size < sizeof(mcp_host_api)) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    *plugin_context = nullptr;

    auto* context = new (std::nothrow) SystemPluginContext{host};

    if (context == nullptr) {
        return MCP_PLUGIN_ERROR;
    }

    *plugin_context = context;

    log_message(
        context,
        MCP_LOG_INFO,
        "System plugin initialized"
    );

    return MCP_PLUGIN_OK;
}

void shutdown(void* plugin_context)
{
    auto* context =
        static_cast<SystemPluginContext*>(plugin_context);

    log_message(
        context,
        MCP_LOG_INFO,
        "System plugin shutting down"
    );

    delete context;
}

mcp_plugin_status list_tools(
    void* plugin_context,
    const mcp_tool_definition** tools,
    size_t* tool_count
)
{
    if (plugin_context == nullptr ||
        tools == nullptr ||
        tool_count == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    *tools = TOOLS;
    *tool_count = sizeof(TOOLS) / sizeof(TOOLS[0]);

    return MCP_PLUGIN_OK;
}

mcp_plugin_status execute_tool(
    void* plugin_context,
    const char* tool_name,
    const char* arguments_json,
    char* result_buffer,
    size_t* result_size
)
{
    if (plugin_context == nullptr ||
        tool_name == nullptr ||
        arguments_json == nullptr ||
        result_size == nullptr) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    if (!valid_empty_arguments(arguments_json)) {
        return MCP_PLUGIN_INVALID_ARGUMENT;
    }

    try {
        std::string result;

        const mcp_plugin_status execution_status =
        execute(tool_name, result);

        if (execution_status != MCP_PLUGIN_OK) {
            return execution_status;
        }

        const size_t required_size = result.size() + 1;

        if (result_buffer == nullptr ||
            *result_size < required_size) {
            *result_size = required_size;
            return MCP_PLUGIN_BUFFER_TOO_SMALL;
        }

        std::memcpy(
            result_buffer,
            result.c_str(),
            required_size
        );

        *result_size = required_size;

        return MCP_PLUGIN_OK;
    } catch (...) {
        return MCP_PLUGIN_ERROR;
    }
}

const mcp_plugin_descriptor DESCRIPTOR = {
    MCP_PLUGIN_ABI_VERSION,
    sizeof(mcp_plugin_descriptor),
    "system",
    "0.1.0",
    "Native FreeBSD system information and diagnostics",
    initialize,
    shutdown,
    list_tools,
    execute_tool
};

} // namespace

extern "C" MCP_PLUGIN_EXPORT
const mcp_plugin_descriptor* mcp_plugin_query(void)
{
    return &DESCRIPTOR;
}
