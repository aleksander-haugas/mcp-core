#include "mcp/plugin_abi.h"

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/queue.h>

#include <devinfo.h>
#include <libusb.h>
#include <libgeom.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <new>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <memory>

namespace {

constexpr const char* PLUGIN_ID = "hardware";

struct HardwarePluginContext {
    const mcp_host_api* host;
};

static const char EMPTY_OBJECT_SCHEMA[] =
    "{\"type\":\"object\",\"properties\":{},"
    "\"additionalProperties\":false}";

static const char* const HARDWARE_CAPABILITIES[] = {
    "hardware.read"
};

static const mcp_tool_definition TOOLS[] = {
    {
        "hardware_thermal_info",
        "Read CPU and ACPI thermal sensor information",
        EMPTY_OBJECT_SCHEMA,
        HARDWARE_CAPABILITIES,
        sizeof(HARDWARE_CAPABILITIES) /
        sizeof(HARDWARE_CAPABILITIES[0])
    },
    {
        "hardware_pci_inventory",
        "Enumerate PCI devices using the native FreeBSD device information API",
        EMPTY_OBJECT_SCHEMA,
        HARDWARE_CAPABILITIES,
        sizeof(HARDWARE_CAPABILITIES) /
        sizeof(HARDWARE_CAPABILITIES[0])
    },
    {
        "hardware_gpu_info",
        "Enumerate PCI display adapters and their hardware identifiers",
        EMPTY_OBJECT_SCHEMA,
        HARDWARE_CAPABILITIES,
        sizeof(HARDWARE_CAPABILITIES) /
        sizeof(HARDWARE_CAPABILITIES[0])
    },
    {
        "hardware_usb_inventory",
        "Enumerate USB devices using native libusb",
        EMPTY_OBJECT_SCHEMA,
        HARDWARE_CAPABILITIES,
        sizeof(HARDWARE_CAPABILITIES) /
        sizeof(HARDWARE_CAPABILITIES[0])
    },
    {
        "hardware_storage_inventory",
        "Enumerate GEOM storage providers and their capacities",
        EMPTY_OBJECT_SCHEMA,
        HARDWARE_CAPABILITIES,
        sizeof(HARDWARE_CAPABILITIES) /
        sizeof(HARDWARE_CAPABILITIES[0])
    }
};

void log_message(
    HardwarePluginContext* context,
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
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;

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

    if (*p++ != '{') {
        return false;
    }

    while (*p == ' ' || *p == '\t' ||
           *p == '\r' || *p == '\n') {
        ++p;
    }

    if (*p++ != '}') {
        return false;
    }

    while (*p == ' ' || *p == '\t' ||
           *p == '\r' || *p == '\n') {
        ++p;
    }

    return *p == '\0';
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
struct DeviceScan {
    std::ostringstream* json;
    bool first;
    size_t count;
    bool gpu_only;
    bool failed;
};

std::string identify_pci_device(
    const std::string& pnpinfo
)
{
    auto get_value = [&pnpinfo](const char* key) {
        const std::string prefix =
        std::string(key) + "=";

        const size_t pos = pnpinfo.find(prefix);

        if (pos == std::string::npos) {
            return std::string();
        }

        const size_t start = pos + prefix.size();
        const size_t end = pnpinfo.find(' ', start);

        return pnpinfo.substr(
            start,
            end == std::string::npos
            ? std::string::npos
            : end - start
        );
    };

    const std::string vendor = get_value("vendor");
    const std::string device = get_value("device");

    if (vendor == "0x1022" && device == "0x15df") {
        return "AMD Platform Security Processor";
    }

    if (vendor == "0x1022" && device == "0x15e2") {
        return "AMD Audio Coprocessor";
    }

    return "";
}

void append_device(
    DeviceScan& scan,
    const devinfo_dev* device
)
{
    if (device == nullptr || device->dd_pnpinfo == nullptr) {
        return;
    }

    const std::string pnpinfo = device->dd_pnpinfo;

    const bool has_vendor =
        pnpinfo.find("vendor=") != std::string::npos;

    const bool has_device =
        pnpinfo.find("device=") != std::string::npos;

    if (!has_vendor || !has_device) {
        return;
    }

    const size_t class_pos = pnpinfo.find("class=");

    std::string class_code;

    if (class_pos != std::string::npos) {
        const size_t start = class_pos + 6;
        const size_t end = pnpinfo.find(' ', start);

        class_code = pnpinfo.substr(
            start,
            end == std::string::npos
                ? std::string::npos
                : end - start
        );
    }

    const bool is_gpu =
        class_code.find("0x03") == 0 ||
        class_code.find("0X03") == 0;

    if (scan.gpu_only && !is_gpu) {
        return;
    }

    const std::string driver =
    device->dd_drivername != nullptr
    ? device->dd_drivername : "";

    const std::string device_label =
    identify_pci_device(pnpinfo);

    const bool driver_attached = !driver.empty();

    if (!scan.first) {
        *scan.json << ",";
    }

    scan.first = false;
    ++scan.count;

    *scan.json
        << "{"
        << "\"name\":\""
        << json_escape(
            device->dd_name != nullptr
                ? device->dd_name : ""
        )
        << "\","
        << "\"description\":\""
        << json_escape(
            device->dd_desc != nullptr
                ? device->dd_desc : ""
        )
        << "\","
        << "\"driver\":\""
        << json_escape(driver)
        << "\","
        << "\"driver_attached\":"
        << (driver_attached ? "true" : "false")
        << ","
        << "\"device_label\":\""
        << json_escape(device_label)
        << "\","
        << "\"location\":\""
        << json_escape(
            device->dd_location != nullptr
                ? device->dd_location : ""
        )
        << "\","
        << "\"pnpinfo\":\""
        << json_escape(pnpinfo)
        << "\","
        << "\"class_code\":\""
        << json_escape(class_code)
        << "\""
        << "}";
}

int scan_device_tree(
    devinfo_dev* parent,
    void* argument
)
{
    auto* scan = static_cast<DeviceScan*>(argument);

    if (parent == nullptr || scan == nullptr) {
        return 0;
    }

    try {
        append_device(*scan, parent);

        return devinfo_foreach_device_child(
            parent,
            scan_device_tree,
            argument
        );
    } catch (...) {
        scan->failed = true;
        return 1;
    }
}

bool make_pci_inventory(std::string& result, bool gpu_only)
{
    if (devinfo_init() != 0) {
        return false;
    }

    std::ostringstream json;

    DeviceScan scan {
        &json,
        true,
        0,
        gpu_only,
        false
    };

    devinfo_dev* root =
        devinfo_handle_to_device(DEVINFO_ROOT_DEVICE);

    if (root == nullptr) {
        devinfo_free();
        return false;
    }

    const int scan_status = devinfo_foreach_device_child(
        root,
        scan_device_tree,
        &scan
    );

    devinfo_free();

    if (scan_status != 0 || scan.failed) {
        return false;
    }

    std::ostringstream output;

    output << "{"
           << "\"device_count\":" << scan.count << ","
           << "\"devices\":["
           << json.str()
           << "]"
           << "}";

    result = output.str();
    return true;
}

bool append_thermal_sensor(
    std::ostringstream& json,
    bool& first,
    const std::string& name,
    const std::string& sysctl_name
)
{
    int32_t raw = 0;

    if (!read_sysctl(sysctl_name.c_str(), raw)) {
        return false;
    }

    const double temperature =
    static_cast<double>(raw) / 10.0 - 273.15;

    if (!std::isfinite(temperature)) {
        return false;
    }

    if (!first) {
        json << ",";
    }

    first = false;

    json << "{"
    << "\"name\":\"" << json_escape(name) << "\","
    << "\"sysctl\":\"" << json_escape(sysctl_name) << "\","
    << "\"raw\":" << raw << ","
    << "\"value\":"
    << std::fixed << std::setprecision(1)
    << temperature << ","
    << "\"unit\":\"C\""
    << "}";

    return true;
}

bool make_thermal_info(std::string& result)
{
    std::ostringstream sensors;
    bool first = true;
    size_t count = 0;

    if (append_thermal_sensor(
            sensors,
            first,
            "AMD CPU thermal sensor",
            "dev.amdtemp.0.core0.sensor0")) {
        ++count;
    }

    if (append_thermal_sensor(
            sensors,
            first,
            "ACPI thermal zone",
            "hw.acpi.thermal.tz0.temperature")) {
        ++count;
    }

    int cpu_count = 0;

    if (read_sysctl("hw.ncpu", cpu_count) &&
        cpu_count > 0 && cpu_count <= 256) {
        for (int cpu = 0; cpu < cpu_count; ++cpu) {
            const std::string name =
                "CPU " + std::to_string(cpu);

            const std::string sysctl_name =
                "dev.cpu." + std::to_string(cpu) +
                ".temperature";

            if (append_thermal_sensor(
                    sensors,
                    first,
                    name,
                    sysctl_name)) {
                ++count;
            }
        }
    }

    std::ostringstream output;

    output << "{"
           << "\"sensor_count\":" << count << ","
           << "\"sensors\":["
           << sensors.str()
           << "]"
           << "}";

    result = output.str();
    return true;
}

struct LibusbContextDeleter {
    void operator()(libusb_context* context) const noexcept
    {
        if (context != nullptr) {
            libusb_exit(context);
        }
    }
};

struct LibusbDeviceListDeleter {
    void operator()(libusb_device** devices) const noexcept
    {
        if (devices != nullptr) {
            libusb_free_device_list(devices, 1);
        }
    }
};

struct GeomMeshDeleter {
    void operator()(gmesh* mesh) const noexcept
    {
        if (mesh != nullptr) {
            geom_deletetree(mesh);
        }
    }
};

bool make_usb_inventory(std::string& result)
{
    libusb_context* context = nullptr;

    const int init_status = libusb_init(&context);

    std::unique_ptr<
        libusb_context,
        LibusbContextDeleter
    > context_guard(context);

    if (init_status != LIBUSB_SUCCESS || context == nullptr) {
        return false;
    }

    libusb_device** devices = nullptr;

    const ssize_t device_count =
        libusb_get_device_list(context, &devices);

    std::unique_ptr<
        libusb_device*,
        LibusbDeviceListDeleter
    > devices_guard(devices);

    if (device_count < 0 || devices_guard.get() == nullptr) {
        return false;
    }

    std::ostringstream json;
    size_t count = 0;

    for (ssize_t i = 0; i < device_count; ++i) {
        libusb_device* device = devices_guard.get()[i];

        if (device == nullptr) {
            continue;
        }

        libusb_device_descriptor descriptor {};

        if (libusb_get_device_descriptor(
                device,
                &descriptor
            ) != LIBUSB_SUCCESS) {
            continue;
        }

        uint8_t ports[8] = {};

        const int port_count = libusb_get_port_numbers(
            device,
            ports,
            sizeof(ports)
        );

        if (count != 0) {
            json << ",";
        }

        json << "{"
             << "\"vendor_id\":\"0x"
             << std::hex << std::setw(4) << std::setfill('0')
             << descriptor.idVendor
             << "\","
             << "\"product_id\":\"0x"
             << std::setw(4)
             << descriptor.idProduct
             << "\","
             << std::dec
             << "\"bus\":"
             << static_cast<unsigned int>(
                    libusb_get_bus_number(device)
                )
             << ","
             << "\"address\":"
             << static_cast<unsigned int>(
                    libusb_get_device_address(device)
                )
             << ","
             << "\"device_class\":"
             << static_cast<unsigned int>(
                    descriptor.bDeviceClass
                )
             << ","
             << "\"device_subclass\":"
             << static_cast<unsigned int>(
                    descriptor.bDeviceSubClass
                )
             << ","
             << "\"device_protocol\":"
             << static_cast<unsigned int>(
                    descriptor.bDeviceProtocol
                )
             << ","
             << "\"usb_version\":\""
             << std::hex
             << std::setw(4) << std::setfill('0')
             << descriptor.bcdUSB
             << std::dec
             << "\","
             << "\"configuration_count\":"
             << static_cast<unsigned int>(
                    descriptor.bNumConfigurations
                )
             << ","
             << "\"port_path\":[";

        if (port_count > 0) {
            for (int port = 0; port < port_count; ++port) {
                if (port != 0) {
                    json << ",";
                }

                json << static_cast<unsigned int>(ports[port]);
            }
        }

        json << "]"
             << "}";

        ++count;
    }

    std::ostringstream output;

    output << "{"
           << "\"device_count\":" << count << ","
           << "\"devices\":["
           << json.str()
           << "]"
           << "}";

    result = output.str();
    return true;
}
bool make_storage_inventory(std::string& result)
{
    struct gmesh mesh {};

    if (geom_gettree(&mesh) != 0) {
        return false;
    }

    std::unique_ptr<gmesh, GeomMeshDeleter> mesh_guard(&mesh);

    std::ostringstream json;
    size_t count = 0;

    gclass* class_entry = nullptr;
    ggeom* geom_entry = nullptr;
    gprovider* provider_entry = nullptr;

    LIST_FOREACH(class_entry, &mesh.lg_class, lg_class) {
        LIST_FOREACH(geom_entry, &class_entry->lg_geom, lg_geom) {
            LIST_FOREACH(
                provider_entry,
                &geom_entry->lg_provider,
                lg_provider
            ) {
                if (provider_entry->lg_name == nullptr) {
                    continue;
                }

                if (count != 0) {
                    json << ",";
                }

                const int64_t media_size =
                    provider_entry->lg_mediasize;

                const int sector_size =
                    provider_entry->lg_sectorsize;

                json << "{"
                     << "\"name\":\""
                     << json_escape(provider_entry->lg_name)
                     << "\","
                     << "\"geom\":\""
                     << json_escape(
                            geom_entry->lg_name != nullptr
                                ? geom_entry->lg_name : ""
                        )
                     << "\","
                     << "\"class\":\""
                     << json_escape(
                            class_entry->lg_name != nullptr
                                ? class_entry->lg_name : ""
                        )
                     << "\","
                     << "\"media_size_bytes\":"
                     << (media_size >= 0 ? media_size : 0)
                     << ","
                     << "\"sector_size_bytes\":"
                     << (sector_size > 0 ? sector_size : 0)
                     << ","
                     << "\"stripe_size_bytes\":"
                     << provider_entry->lg_stripesize
                     << ","
                     << "\"stripe_offset_bytes\":"
                     << provider_entry->lg_stripeoffset
                     << "}";

                ++count;
            }
        }
    }

    std::ostringstream output;

    output << "{"
           << "\"provider_count\":" << count << ","
           << "\"providers\":["
           << json.str()
           << "]"
           << "}";

    result = output.str();
    return true;
}

mcp_plugin_status execute(
    const char* tool_name,
    std::string& result
)
{
    if (std::strcmp(tool_name, "hardware_thermal_info") == 0) {
        return make_thermal_info(result)
            ? MCP_PLUGIN_OK : MCP_PLUGIN_ERROR;
    }

    if (std::strcmp(tool_name, "hardware_pci_inventory") == 0) {
        return make_pci_inventory(result, false)
            ? MCP_PLUGIN_OK : MCP_PLUGIN_ERROR;
    }

    if (std::strcmp(tool_name, "hardware_gpu_info") == 0) {
        return make_pci_inventory(result, true)
            ? MCP_PLUGIN_OK : MCP_PLUGIN_ERROR;
    }

    if (std::strcmp(tool_name, "hardware_usb_inventory") == 0) {
        return make_usb_inventory(result)
            ? MCP_PLUGIN_OK : MCP_PLUGIN_ERROR;
    }

    if (std::strcmp(tool_name, "hardware_storage_inventory") == 0) {
        return make_storage_inventory(result)
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

    auto* context = new (std::nothrow) HardwarePluginContext{host};

    if (context == nullptr) {
        return MCP_PLUGIN_ERROR;
    }

    *plugin_context = context;

    log_message(
        context,
        MCP_LOG_INFO,
        "Hardware plugin initialized"
    );

    return MCP_PLUGIN_OK;
}

void shutdown(void* plugin_context)
{
    auto* context =
        static_cast<HardwarePluginContext*>(plugin_context);

    log_message(
        context,
        MCP_LOG_INFO,
        "Hardware plugin shutting down"
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
    "hardware",
    "0.1.0",
    "Native read-only FreeBSD hardware inventory and diagnostics",
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
