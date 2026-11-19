#ifndef MCP_REQUEST_HANDLER_HPP
#define MCP_REQUEST_HANDLER_HPP

#include "mcp/plugin_manager.hpp"
#include "mcp/authorization.hpp"
#include <string>

namespace mcp {

class RequestHandler {
public:
    RequestHandler(
        PluginManager& plugin_manager,
        const AuthorizationPolicy& authorization_policy,
        PeerIdentity peer_identity
    );

    RequestHandler(const RequestHandler&) = delete;
    RequestHandler& operator=(const RequestHandler&) = delete;

    // Procesa un mensaje JSON-RPC.
    //
    // Devuelve:
    // - JSON-RPC response si existe una respuesta.
    // - Cadena vacía para notificaciones.
    std::string handle(const std::string& input);

    // Estado de inicialización MCP.
    bool initialized() const;

    // Reinicia el estado de la sesión.
    void reset();

private:
    PluginManager& plugin_manager_;

    const AuthorizationPolicy& authorization_policy_;
    PeerIdentity peer_identity_;

    bool initialize_response_sent_ = false;
    bool initialized_ = false;

    std::string handle_initialize(
        const std::string& id_json,
        const std::string& params_json
    );

    std::string handle_ping(
        const std::string& id_json
    );

    std::string handle_tools_list(
        const std::string& id_json
    );

    std::string handle_tools_call(
        const std::string& id_json,
        const std::string& params_json
    );

    std::string make_tool_error(
        const std::string& message
    ) const;
};

} // namespace mcp

#endif
