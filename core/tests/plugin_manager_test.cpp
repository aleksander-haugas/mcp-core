#include "mcp/plugin_manager.hpp"

#include <iostream>
#include <string>

int main()
{
    mcp::PluginManager manager;

    std::cout << "========== CARGAR PLUGINS ==========" << std::endl;

    if (!manager.load_directory("core/tests/plugins")) {
        std::cerr << "ERROR: no se pudo explorar el directorio de plugins"
                  << std::endl;
        return 1;
    }

    std::cout << "Plugins cargados: "
              << manager.plugin_count() << std::endl;

    std::cout << "Herramientas registradas: "
              << manager.tool_count() << std::endl;

    if (manager.plugin_count() != 1 || manager.tool_count() != 1) {
        std::cerr << "ERROR: cantidades inesperadas de plugins o herramientas"
                  << std::endl;
        manager.shutdown();
        return 1;
    }

    std::cout << std::endl;
    std::cout << "========== REGISTRO DE HERRAMIENTAS =========="
              << std::endl;

    for (const auto& tool : manager.tools()) {
        std::cout << "Plugin: " << tool.plugin_id << std::endl;
        std::cout << "Nombre: " << tool.name << std::endl;
        std::cout << "Descripción: " << tool.description << std::endl;
        std::cout << "JSON Schema: " << tool.input_schema_json << std::endl;
    }

    std::cout << std::endl;
    std::cout << "========== EJECUTAR HERRAMIENTA =========="
              << std::endl;

    std::string result;
    std::string error;

    if (!manager.execute_tool("core_test_echo", "{}", result, error)) {
        std::cerr << "ERROR: ejecución fallida: " << error << std::endl;
        manager.shutdown();
        return 1;
    }

    std::cout << "Resultado: " << result << std::endl;

    const std::string expected =
        "{\"message\":\"hello from test plugin\"}";

    if (result != expected) {
        std::cerr << "ERROR: resultado inesperado" << std::endl;
        manager.shutdown();
        return 1;
    }

    std::cout << std::endl;
    std::cout << "========== CERRAR PLUGINS =========="
              << std::endl;

    manager.shutdown();

    if (manager.plugin_count() != 0 || manager.tool_count() != 0) {
        std::cerr << "ERROR: el registro no quedó vacío tras shutdown"
                  << std::endl;
        return 1;
    }

    std::cout << "Plugins restantes: " << manager.plugin_count() << std::endl;
    std::cout << "Herramientas restantes: " << manager.tool_count()
              << std::endl;

    std::cout << std::endl;
    std::cout << "PLUGIN MANAGER INTEGRATION TEST: PASS" << std::endl;

    return 0;
}
