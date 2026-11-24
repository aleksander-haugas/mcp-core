package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/heroiclabs/nakama-common/api"
	"github.com/heroiclabs/nakama-common/runtime"
)

const (
	agentStreamMode  uint8 = 123
	agentStreamLabel       = "mcp_agents"

	maxMessageSize            = 4 * 1024 * 1024
	maxWebSocketMessageSize   = 4 * 1024 * 1024
	maxIdentifierSize         = 256
	maxCapabilities           = 64
	groupHistoryPageSize      = 100
	groupHistoryMaxScan       = 1000
	groupHistoryMaxPages      = 20
	groupHistoryMaxCursorSize = 32768
)

type agentIdentity struct {
	AgentID    string `json:"agent_id"`
	InstanceID string `json:"instance_id"`
}

type agentMetadata struct {
	UserID             string   `json:"user_id"`
	AgentID            string   `json:"agent_id"`
	InstanceID         string   `json:"instance_id"`
	Capabilities       []string `json:"capabilities"`
	MaxConcurrentTasks int      `json:"max_concurrent_tasks"`
}

type registerRequest struct {
	AgentID            string   `json:"agent_id"`
	InstanceID         string   `json:"instance_id"`
	Capabilities       []string `json:"capabilities"`
	MaxConcurrentTasks int      `json:"max_concurrent_tasks"`
}

type unregisterRequest struct {
	AgentID    string `json:"agent_id"`
	InstanceID string `json:"instance_id"`
}

type discoverResponse struct {
	Agents []agentMetadata `json:"agents"`
}

type successResponse struct {
	Success bool `json:"success"`
}

type agentEnvelope struct {
	Protocol      string          `json:"protocol"`
	Version       int             `json:"version"`
	Type          string          `json:"type"`
	GroupID       string          `json:"group_id,omitempty"`
	MessageID     string          `json:"message_id"`
	Source        agentIdentity   `json:"source"`
	Target        *agentIdentity  `json:"target,omitempty"`
	CorrelationID *string         `json:"correlation_id,omitempty"`
	Method        *string         `json:"method,omitempty"`
	Payload       json.RawMessage `json:"payload"`
	Timestamp     string          `json:"timestamp"`
}

var registrationMu sync.Mutex

// InitModule is the Nakama Go runtime module entry point.
func InitModule(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	initializer runtime.Initializer,
) error {
	if err := initializer.RegisterRpc("mcp_agents_register", registerAgent); err != nil {
		return fmt.Errorf("register mcp_agents_register: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_unregister", unregisterAgent); err != nil {
		return fmt.Errorf("register mcp_agents_unregister: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_discover", discoverAgents); err != nil {
		return fmt.Errorf("register mcp_agents_discover: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_send", sendAgentMessage); err != nil {
		return fmt.Errorf("register mcp_agents_send: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_group_create", groupCreate); err != nil {
		return fmt.Errorf("register mcp_agents_group_create: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_group_discover", groupDiscover); err != nil {
		return fmt.Errorf("register mcp_agents_group_discover: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_group_get", groupGet); err != nil {
		return fmt.Errorf("register mcp_agents_group_get: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_group_join", groupJoin); err != nil {
		return fmt.Errorf("register mcp_agents_group_join: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_group_leave", groupLeave); err != nil {
		return fmt.Errorf("register mcp_agents_group_leave: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_group_invite", groupInvite); err != nil {
		return fmt.Errorf("register mcp_agents_group_invite: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_group_kick", groupKick); err != nil {
		return fmt.Errorf("register mcp_agents_group_kick: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_group_finish", groupFinish); err != nil {
		return fmt.Errorf("register mcp_agents_group_finish: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_group_send", groupSend); err != nil {
		return fmt.Errorf("register mcp_agents_group_send: %w", err)
	}

	if err := initializer.RegisterRpc("mcp_agents_group_history", groupHistory); err != nil {
		return fmt.Errorf("register mcp_agents_group_history: %w", err)
	}

	if err := initializer.RegisterEventSessionEnd(cleanupSession); err != nil {
		return fmt.Errorf("register session end callback: %w", err)
	}

	logger.Info("MCP Agents runtime module initialized")
	return nil
}

func validIdentifier(value string) bool {
	if value == "" || len(value) > maxIdentifierSize {
		return false
	}

	// Match the C++ protocol validator: byte-based identifier checks.
	for i := 0; i < len(value); i++ {
		if value[i] < 0x20 || value[i] == 0x7f {
			return false
		}
	}

	return true
}

func validateIdentity(identity agentIdentity) error {
	if !validIdentifier(identity.AgentID) {
		return errors.New("invalid agent_id")
	}
	if !validIdentifier(identity.InstanceID) {
		return errors.New("invalid instance_id")
	}
	return nil
}

func validateCapabilities(capabilities []string) error {
	if len(capabilities) > maxCapabilities {
		return errors.New("too many capabilities")
	}

	seen := make(map[string]struct{}, len(capabilities))

	for _, capability := range capabilities {
		if !validIdentifier(capability) {
			return errors.New("invalid capability identifier")
		}

		if _, exists := seen[capability]; exists {
			return fmt.Errorf("duplicate capability: %s", capability)
		}

		seen[capability] = struct{}{}
	}

	return nil
}

func decodeJSON(payload string, destination interface{}) error {
	if len(payload) == 0 || len(payload) > maxMessageSize {
		return errors.New("invalid JSON payload size")
	}

	decoder := json.NewDecoder(strings.NewReader(payload))
	decoder.DisallowUnknownFields()

	if err := decoder.Decode(destination); err != nil {
		return fmt.Errorf("invalid JSON: %w", err)
	}

	var extra interface{}
	if err := decoder.Decode(&extra); err != io.EOF {
		if err == nil {
			return errors.New("unexpected trailing JSON data")
		}
		return fmt.Errorf("invalid trailing JSON data: %w", err)
	}

	return nil
}

func authenticatedSession(ctx context.Context) (string, string, error) {
	userID, ok := ctx.Value(runtime.RUNTIME_CTX_USER_ID).(string)
	if !ok || userID == "" {
		return "", "", errors.New("authenticated Nakama user_id is unavailable")
	}

	sessionID, ok := ctx.Value(runtime.RUNTIME_CTX_SESSION_ID).(string)
	if !ok || sessionID == "" {
		return "", "", errors.New("authenticated Nakama session_id is unavailable")
	}

	return userID, sessionID, nil
}

func encodeMetadata(metadata agentMetadata) (string, error) {
	data, err := json.Marshal(metadata)
	if err != nil {
		return "", fmt.Errorf("encode agent metadata: %w", err)
	}

	return string(data), nil
}

func decodeMetadata(status string) (agentMetadata, error) {
	var metadata agentMetadata

	if status == "" {
		return metadata, errors.New("presence has no agent metadata")
	}

	if err := decodeJSON(status, &metadata); err != nil {
		return metadata, err
	}

	identity := agentIdentity{
		AgentID:    metadata.AgentID,
		InstanceID: metadata.InstanceID,
	}

	if err := validateIdentity(identity); err != nil {
		return metadata, err
	}

	if err := validateCapabilities(metadata.Capabilities); err != nil {
		return metadata, err
	}

	if metadata.MaxConcurrentTasks < 1 || metadata.MaxConcurrentTasks > 1024 {
		return metadata, errors.New("invalid max_concurrent_tasks")
	}

	return metadata, nil
}

func listAgentPresences(nk runtime.NakamaModule) ([]runtime.Presence, error) {
	presences, err := nk.StreamUserList(
		agentStreamMode,
		"",
		"",
		agentStreamLabel,
		false,
		true,
	)
	if err != nil {
		return nil, fmt.Errorf("list agent presences: %w", err)
	}

	return presences, nil
}

func registerAgent(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, sessionID, err := authenticatedSession(ctx)
	if err != nil {
		return "", err
	}

	var request registerRequest
	if err := decodeJSON(payload, &request); err != nil {
		return "", err
	}

	identity := agentIdentity{
		AgentID:    request.AgentID,
		InstanceID: request.InstanceID,
	}

	if err := validateIdentity(identity); err != nil {
		return "", err
	}

	if err := validateCapabilities(request.Capabilities); err != nil {
		return "", err
	}

	if request.MaxConcurrentTasks < 1 || request.MaxConcurrentTasks > 1024 {
		return "", errors.New("max_concurrent_tasks must be between 1 and 1024")
	}

	metadata := agentMetadata{
		AgentID:            identity.AgentID,
		InstanceID:         identity.InstanceID,
		Capabilities:       request.Capabilities,
		MaxConcurrentTasks: request.MaxConcurrentTasks,
	}

	status, err := encodeMetadata(metadata)
	if err != nil {
		return "", err
	}

	registrationMu.Lock()
	defer registrationMu.Unlock()

	presences, err := listAgentPresences(nk)
	if err != nil {
		return "", err
	}

	// One active agent per Nakama account.
	for _, presence := range presences {
		if presence.GetUserId() != userID {
			continue
		}

		if err := nk.StreamUserLeave(
			agentStreamMode,
			"",
			"",
			agentStreamLabel,
			presence.GetUserId(),
			presence.GetSessionId(),
		); err != nil {
			return "", fmt.Errorf("remove previous agent presence: %w", err)
		}
	}

	joined, err := nk.StreamUserJoin(
		agentStreamMode,
		"",
		"",
		agentStreamLabel,
		userID,
		sessionID,
		false,
		false,
		status,
	)
	if err != nil {
		return "", fmt.Errorf("join agent stream: %w", err)
	}

	if !joined {
		return "", errors.New("Nakama rejected agent stream registration")
	}

	logger.Info(
		"Agent registered: user_id=%s agent_id=%s instance_id=%s",
		userID,
		identity.AgentID,
		identity.InstanceID,
	)

	response := struct {
		Success bool          `json:"success"`
		Agent   agentIdentity `json:"agent"`
	}{
		Success: true,
		Agent:   identity,
	}

	data, err := json.Marshal(response)
	if err != nil {
		return "", err
	}

	return string(data), nil
}

func unregisterAgent(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, sessionID, err := authenticatedSession(ctx)
	if err != nil {
		return "", err
	}

	var request unregisterRequest
	if err := decodeJSON(payload, &request); err != nil {
		return "", err
	}

	identity := agentIdentity{
		AgentID:    request.AgentID,
		InstanceID: request.InstanceID,
	}

	if err := validateIdentity(identity); err != nil {
		return "", err
	}

	registrationMu.Lock()
	defer registrationMu.Unlock()

	presence, err := nk.StreamUserGet(
		agentStreamMode,
		"",
		"",
		agentStreamLabel,
		userID,
		sessionID,
	)
	if err != nil {
		return "", fmt.Errorf("get current agent presence: %w", err)
	}

	if presence == nil {
		return "", errors.New("agent is not registered in this session")
	}

	metadata, err := decodeMetadata(presence.GetStatus())
	if err != nil {
		return "", err
	}

	if metadata.AgentID != identity.AgentID ||
		metadata.InstanceID != identity.InstanceID {
		return "", errors.New("agent identity does not match registered identity")
	}

	if err := nk.StreamUserLeave(
		agentStreamMode,
		"",
		"",
		agentStreamLabel,
		userID,
		sessionID,
	); err != nil {
		return "", fmt.Errorf("leave agent stream: %w", err)
	}

	logger.Info(
		"Agent unregistered: user_id=%s agent_id=%s",
		userID,
		identity.AgentID,
	)

	data, _ := json.Marshal(successResponse{Success: true})
	return string(data), nil
}

func discoverAgents(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	if _, _, err := authenticatedSession(ctx); err != nil {
		return "", err
	}

	var request map[string]json.RawMessage

	if err := decodeJSON(payload, &request); err != nil {
		return "", err
	}

	if request == nil || len(request) != 0 {
		return "", errors.New("discover request must be an empty JSON object")
	}

	presences, err := listAgentPresences(nk)
	if err != nil {
		return "", err
	}

	response := discoverResponse{
		Agents: make([]agentMetadata, 0, len(presences)),
	}

	for _, presence := range presences {
		metadata, err := decodeMetadata(presence.GetStatus())
		if err != nil {
			logger.Warn(
				"Ignoring invalid agent presence: user_id=%s session_id=%s error=%v",
				presence.GetUserId(),
				presence.GetSessionId(),
				err,
			)
			continue
		}

		metadata.UserID = presence.GetUserId()
		response.Agents = append(response.Agents, metadata)
	}

	sort.Slice(response.Agents, func(i, j int) bool {
		if response.Agents[i].AgentID != response.Agents[j].AgentID {
			return response.Agents[i].AgentID < response.Agents[j].AgentID
		}

		return response.Agents[i].InstanceID < response.Agents[j].InstanceID
	})

	data, err := json.Marshal(response)
	if err != nil {
		return "", err
	}

	return string(data), nil
}

// scanJSONValue validates one JSON value and rejects duplicate object keys.
// Keys are compared after JSON unescaping, matching the C++ protocol parser.
func scanJSONValue(decoder *json.Decoder, token json.Token, depth int) error {
	switch value := token.(type) {
	case json.Delim:
		switch value {
		case '{':
			depth++
			if depth > 128 {
				return errors.New("JSON nesting depth exceeds 128 levels")
			}

			keys := make(map[string]struct{})
			for decoder.More() {
				keyToken, err := decoder.Token()
				if err != nil {
					return err
				}

				key, ok := keyToken.(string)
				if !ok {
					return errors.New("invalid JSON object key")
				}

				if _, exists := keys[key]; exists {
					return fmt.Errorf("duplicate JSON object key: %s", key)
				}
				keys[key] = struct{}{}

				if err := scanNextJSONValue(decoder, depth); err != nil {
					return err
				}
			}

			closing, err := decoder.Token()
			if err != nil || closing != json.Delim('}') {
				return errors.New("unterminated JSON object")
			}

		case '[':
			depth++
			if depth > 128 {
				return errors.New("JSON nesting depth exceeds 128 levels")
			}

			for decoder.More() {
				if err := scanNextJSONValue(decoder, depth); err != nil {
					return err
				}
			}

			closing, err := decoder.Token()
			if err != nil || closing != json.Delim(']') {
				return errors.New("unterminated JSON array")
			}

		default:
			return errors.New("unexpected JSON delimiter")
		}
	}
	return nil
}

func scanNextJSONValue(decoder *json.Decoder, depth int) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	return scanJSONValue(decoder, token, depth)
}

// validateProtocolJSON rejects duplicate keys, malformed JSON, trailing
// documents and nesting deeper than the C++ Agent Protocol limit.
func validateProtocolJSON(payload string) error {
	decoder := json.NewDecoder(strings.NewReader(payload))
	decoder.UseNumber()

	first, err := decoder.Token()
	if err != nil {
		return fmt.Errorf("invalid JSON: %w", err)
	}

	if first != json.Delim('{') {
		return errors.New("agent message must be a JSON object")
	}

	if err := scanJSONValue(decoder, first, 0); err != nil {
		return fmt.Errorf("invalid JSON document: %w", err)
	}

	if _, err := decoder.Token(); err != io.EOF {
		if err == nil {
			return errors.New("unexpected trailing data after JSON document")
		}
		return fmt.Errorf("invalid trailing JSON data: %w", err)
	}

	return nil
}

// validMethod mirrors the C++ method grammar:
// at least two non-empty dot-separated segments; each segment starts
// with an ASCII letter or underscore, followed by letters, digits,
// underscores or hyphens.
func validMethod(value string) bool {
	if len(value) == 0 || len(value) > maxIdentifierSize {
		return false
	}

	segmentStart := true
	segments := 0

	for i := 0; i < len(value); i++ {
		ch := value[i]

		if ch == '.' {
			if segmentStart {
				return false
			}
			segmentStart = true
			segments++
			continue
		}

		if segmentStart {
			if !((ch >= 'A' && ch <= 'Z') ||
				(ch >= 'a' && ch <= 'z') || ch == '_') {
				return false
			}
			segmentStart = false
			continue
		}

		if !((ch >= 'A' && ch <= 'Z') ||
			(ch >= 'a' && ch <= 'z') ||
			(ch >= '0' && ch <= '9') ||
			ch == '_' || ch == '-') {
			return false
		}
	}

	return !segmentStart && segments >= 1
}

// validProtocolTimestamp mirrors the C++ UTC RFC3339 validator.
// It deliberately does not use time.Parse because the C++ contract
// permits arbitrary fractional-second precision.
func validProtocolTimestamp(value string) bool {
	if len(value) < 20 {
		return false
	}

	if value[4] != '-' || value[7] != '-' ||
		value[10] != 'T' || value[13] != ':' ||
		value[16] != ':' {
		return false
	}

	digit := func(start, count int) bool {
		if start+count > len(value) {
			return false
		}
		for i := start; i < start+count; i++ {
			if value[i] < '0' || value[i] > '9' {
				return false
			}
		}
		return true
	}

	if !digit(0, 4) || !digit(5, 2) || !digit(8, 2) ||
		!digit(11, 2) || !digit(14, 2) || !digit(17, 2) {
		return false
	}

	year := int(value[0]-'0')*1000 +
		int(value[1]-'0')*100 +
		int(value[2]-'0')*10 +
		int(value[3]-'0')

	month := int(value[5]-'0')*10 + int(value[6]-'0')
	day := int(value[8]-'0')*10 + int(value[9]-'0')
	hour := int(value[11]-'0')*10 + int(value[12]-'0')
	minute := int(value[14]-'0')*10 + int(value[15]-'0')
	second := int(value[17]-'0')*10 + int(value[18]-'0')

	if year == 0 || month < 1 || month > 12 ||
		hour > 23 || minute > 59 || second > 59 {
		return false
	}

	// time.Date normalizes invalid dates, so compare the components.
	parsed := time.Date(year, time.Month(month), day,
		hour, minute, second, 0, time.UTC)

	if parsed.Year() != year || int(parsed.Month()) != month ||
		parsed.Day() != day {
		return false
	}

	pos := 19
	if pos < len(value) && value[pos] == '.' {
		pos++
		fractionStart := pos

		for pos < len(value) && value[pos] >= '0' && value[pos] <= '9' {
			pos++
		}

		if pos == fractionStart {
			return false
		}
	}

	return pos+1 == len(value) && value[pos] == 'Z'
}

func validateErrorPayload(payload json.RawMessage) error {
	var object map[string]json.RawMessage
	if err := json.Unmarshal(payload, &object); err != nil || object == nil {
		return errors.New("error payload must be a JSON object")
	}

	var code string
	rawCode, exists := object["code"]
	if !exists || json.Unmarshal(rawCode, &code) != nil {
		return errors.New("error payload requires string code")
	}

	if len(code) == 0 || len(code) > 64 ||
		code[0] < 'A' || code[0] > 'Z' {
		return errors.New("invalid error code")
	}

	for i := 0; i < len(code); i++ {
		ch := code[i]
		if !((ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') || ch == '_') {
			return errors.New("invalid error code")
		}
	}

	var message string
	rawMessage, exists := object["message"]
	if !exists || json.Unmarshal(rawMessage, &message) != nil ||
		len(message) == 0 || len(message) > 4096 {
		return errors.New("invalid error message")
	}

	if details, exists := object["details"]; exists {
		var detailsObject map[string]json.RawMessage
		if err := json.Unmarshal(details, &detailsObject); err != nil ||
			detailsObject == nil {
			return errors.New("error details must be a JSON object")
		}
	}

	return nil
}

func validateEnvelope(payload string) (agentEnvelope, error) {
	var envelope agentEnvelope

	if len(payload) == 0 || len(payload) > maxMessageSize {
		return envelope, errors.New("invalid agent message size")
	}

	if err := validateProtocolJSON(payload); err != nil {
		return envelope, err
	}

	// Unknown envelope fields are intentionally ignored, as in C++.
	if err := json.Unmarshal([]byte(payload), &envelope); err != nil {
		return envelope, fmt.Errorf("invalid agent envelope: %w", err)
	}

	// Preserve the C++ parser distinction between absent optional fields
	// and fields explicitly present with null or an invalid JSON type.
	var rawFields map[string]json.RawMessage
	if err := json.Unmarshal([]byte(payload), &rawFields); err != nil {
		return envelope, fmt.Errorf("invalid agent envelope fields: %w", err)
	}
	if rawFields == nil {
		return envelope, errors.New("agent envelope must be a JSON object")
	}

	if raw, present := rawFields["target"]; present {
		var targetObject map[string]json.RawMessage
		if err := json.Unmarshal(raw, &targetObject); err != nil || targetObject == nil {
			return envelope, errors.New("target must be a JSON object")
		}
	}

	for _, field := range []string{"correlation_id", "method"} {
		if raw, present := rawFields[field]; present {
			var value string
			if err := json.Unmarshal(raw, &value); err != nil {
				return envelope, fmt.Errorf("%s must be a JSON string", field)
			}
		}
	}

	if envelope.Protocol != "agent" || envelope.Version != 1 {
		return envelope, errors.New("unsupported agent protocol or version")
	}

	if !validIdentifier(envelope.MessageID) {
		return envelope, errors.New("invalid message_id")
	}

	if envelope.GroupID != "" && !validIdentifier(envelope.GroupID) {
		return envelope, errors.New("invalid group_id")
	}

	if err := validateIdentity(envelope.Source); err != nil {
		return envelope, fmt.Errorf("invalid source: %w", err)
	}

	if envelope.Target != nil {
		if err := validateIdentity(*envelope.Target); err != nil {
			return envelope, fmt.Errorf("invalid target: %w", err)
		}
	}

	if len(envelope.Payload) == 0 {
		return envelope, errors.New("missing message payload")
	}

	if !validProtocolTimestamp(envelope.Timestamp) {
		return envelope, errors.New("invalid UTC RFC3339 timestamp")
	}

	hasMethod := envelope.Method != nil
	hasCorrelation := envelope.CorrelationID != nil
	hasTarget := envelope.Target != nil

	if hasMethod && !validMethod(*envelope.Method) {
		return envelope, errors.New("invalid method")
	}

	if hasCorrelation && !validIdentifier(*envelope.CorrelationID) {
		return envelope, errors.New("invalid correlation_id")
	}

	switch envelope.Type {
	case "request":
		if !hasTarget || !hasMethod || hasCorrelation {
			return envelope, errors.New(
				"request requires target and method, without correlation_id")
		}

	case "response":
		if !hasTarget || !hasCorrelation || hasMethod {
			return envelope, errors.New(
				"response requires target and correlation_id, without method")
		}

	case "event":
		if !hasMethod || hasCorrelation {
			return envelope, errors.New(
				"event requires method and cannot contain correlation_id")
		}

	case "error":
		if !hasTarget || !hasCorrelation || hasMethod {
			return envelope, errors.New(
				"error requires target and correlation_id, without method")
		}
		if err := validateErrorPayload(envelope.Payload); err != nil {
			return envelope, err
		}

	default:
		return envelope, errors.New("unsupported agent message type")
	}

	return envelope, nil
}

func validateGroupEnvelope(payload string) (agentEnvelope, error) {
	envelope, err := validateEnvelope(payload)
	if err != nil {
		return envelope, err
	}

	if envelope.GroupID == "" {
		return envelope, errors.New("group message requires group_id")
	}
	if envelope.Type != "event" {
		return envelope, errors.New("group message must have type event")
	}
	if envelope.Target != nil {
		return envelope, errors.New("group message cannot contain target")
	}
	if envelope.CorrelationID != nil {
		return envelope, errors.New("group message cannot contain correlation_id")
	}
	if envelope.Method == nil || *envelope.Method == "" {
		return envelope, errors.New("group message requires method")
	}

	return envelope, nil
}

type agentRelay struct {
	Protocol  string          `json:"protocol"`
	Version   int             `json:"version"`
	UserID    string          `json:"user_id"`
	SessionID string          `json:"session_id"`
	Envelope  json.RawMessage `json:"envelope"`
}

// encodeAgentRelay adds the authenticated Nakama session identity.
// Identity values come from authenticatedSession, never from the client envelope.
func encodeAgentRelay(userID, sessionID, payload string) (string, error) {
	if userID == "" || len(userID) > maxIdentifierSize {
		return "", errors.New("invalid authenticated user ID for relay")
	}
	if sessionID == "" || len(sessionID) > maxIdentifierSize {
		return "", errors.New("invalid authenticated session ID for relay")
	}
	if payload == "" || len(payload) > maxMessageSize || !json.Valid([]byte(payload)) {
		return "", errors.New("invalid or oversized Agent envelope for relay")
	}

	relay := agentRelay{
		Protocol:  "mcp_agents_relay",
		Version:   1,
		UserID:    userID,
		SessionID: sessionID,
		Envelope:  json.RawMessage(payload),
	}

	encoded, err := json.Marshal(relay)
	if err != nil {
		return "", fmt.Errorf("marshal authenticated relay: %w", err)
	}
	if len(encoded) > maxWebSocketMessageSize {
		return "", errors.New("authenticated relay exceeds the 4 MiB WebSocket message limit")
	}

	return string(encoded), nil
}

func sendAgentMessage(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, sessionID, err := authenticatedSession(ctx)
	if err != nil {
		return "", err
	}

	envelope, err := validateEnvelope(payload)
	if err != nil {
		return "", fmt.Errorf("invalid agent message: %w", err)
	}

	if envelope.GroupID != "" {
		return "", errors.New("group messages must use group_send")
	}

	if envelope.Target == nil {
		return "", errors.New("agent message requires a target for relay")
	}

	presences, err := listAgentPresences(nk)
	if err != nil {
		return "", err
	}

	var sourceFound bool
	var targetPresence runtime.Presence
	var targetMatches int

	for _, presence := range presences {
		metadata, err := decodeMetadata(presence.GetStatus())
		if err != nil {
			continue
		}

		if presence.GetUserId() == userID &&
			presence.GetSessionId() == sessionID {
			if metadata.AgentID != envelope.Source.AgentID ||
				metadata.InstanceID != envelope.Source.InstanceID {
				return "", errors.New(
					"message source does not match authenticated agent",
				)
			}

			sourceFound = true
		}

		if metadata.AgentID == envelope.Target.AgentID &&
			metadata.InstanceID == envelope.Target.InstanceID {
			targetPresence = presence
			targetMatches++
		}
	}

	if !sourceFound {
		return "", errors.New(
			"authenticated session has no active agent registration",
		)
	}

	if targetMatches == 0 {
		return "", errors.New("target agent is not currently available")
	}

	if targetMatches > 1 {
		return "", errors.New("target agent identity is ambiguous")
	}

	if targetPresence.GetUserId() == userID &&
		targetPresence.GetSessionId() == sessionID {
		return "", errors.New(
			"sending a message to the same agent session is not supported",
		)
	}

	relayPayload, err := encodeAgentRelay(userID, sessionID, payload)
	if err != nil {
		return "", fmt.Errorf("encode agent relay: %w", err)
	}

	if err := nk.StreamSend(
		agentStreamMode,
		"",
		"",
		agentStreamLabel,
		relayPayload,
		[]runtime.Presence{targetPresence},
		true,
	); err != nil {
		return "", fmt.Errorf("relay agent message: %w", err)
	}

	logger.Debug(
		"Agent message relayed: source=%s target=%s message_id=%s",
		envelope.Source.AgentID,
		envelope.Target.AgentID,
		envelope.MessageID,
	)

	data, _ := json.Marshal(successResponse{Success: true})
	return string(data), nil
}

// deliverGroupMessageBestEffort emits a committed group message to currently
// connected agent sessions. Storage remains authoritative if realtime delivery
// fails or the group becomes inactive before emission.
func deliverGroupMessageBestEffort(
	ctx context.Context,
	logger runtime.Logger,
	nk runtime.NakamaModule,
	userID string,
	sessionID string,
	payload string,
	envelope agentEnvelope,
) int {
	group, err := loadGroup(ctx, nk, envelope.GroupID)
	if err != nil {
		logger.Debug(
			"Group message committed without realtime delivery: group reload failed group_id=%s message_id=%s error=%v",
			envelope.GroupID, envelope.MessageID, err,
		)
		return 0
	}
	if err := ensureGroupActive(group); err != nil {
		logger.Debug(
			"Group message committed without realtime delivery: group is no longer active group_id=%s message_id=%s error=%v",
			envelope.GroupID, envelope.MessageID, err,
		)
		return 0
	}

	member, role, _, err := groupMembership(
		ctx, nk, envelope.GroupID, userID,
	)
	if err != nil {
		logger.Debug(
			"Group message committed without realtime delivery: sender membership revalidation failed group_id=%s message_id=%s error=%v",
			envelope.GroupID, envelope.MessageID, err,
		)
		return 0
	}
	if !member || (role != "SUPERADMIN" &&
		role != "ADMIN" && role != "MEMBER") {
		logger.Debug(
			"Group message committed without realtime delivery: sender membership changed group_id=%s message_id=%s",
			envelope.GroupID, envelope.MessageID,
		)
		return 0
	}

	_, access, err := readGroupAccess(
		ctx, nk, envelope.GroupID, userID,
	)
	if err != nil {
		logger.Debug(
			"Group message committed without realtime delivery: sender access revalidation failed group_id=%s message_id=%s error=%v",
			envelope.GroupID, envelope.MessageID, err,
		)
		return 0
	}
	if access.JoinedAt == "" || access.RevokedAt != "" {
		logger.Debug(
			"Group message committed without realtime delivery: sender access revoked group_id=%s message_id=%s",
			envelope.GroupID, envelope.MessageID,
		)
		return 0
	}

	memberIDs, err := groupMemberUserIDs(ctx, nk, envelope.GroupID)
	if err != nil {
		logger.Debug(
			"Group message committed without realtime delivery: member refresh failed group_id=%s message_id=%s error=%v",
			envelope.GroupID, envelope.MessageID, err,
		)
		return 0
	}
	if _, ok := memberIDs[userID]; !ok {
		logger.Debug(
			"Group message committed without realtime delivery: sender absent from refreshed membership group_id=%s message_id=%s",
			envelope.GroupID, envelope.MessageID,
		)
		return 0
	}

	presences, err := listAgentPresences(nk)
	if err != nil {
		logger.Debug(
			"Group message committed without realtime delivery: presence refresh failed group_id=%s message_id=%s error=%v",
			envelope.GroupID, envelope.MessageID, err,
		)
		return 0
	}

	sourceFound := false
	for _, presence := range presences {
		if presence.GetUserId() != userID ||
			presence.GetSessionId() != sessionID {
			continue
		}

		metadata, metadataErr := decodeMetadata(presence.GetStatus())
		if metadataErr != nil {
			logger.Debug(
				"Group message committed without realtime delivery: sender presence metadata invalid group_id=%s message_id=%s error=%v",
				envelope.GroupID, envelope.MessageID, metadataErr,
			)
			return 0
		}
		if metadata.AgentID != envelope.Source.AgentID ||
			metadata.InstanceID != envelope.Source.InstanceID {
			logger.Debug(
				"Group message committed without realtime delivery: sender agent identity changed group_id=%s message_id=%s",
				envelope.GroupID, envelope.MessageID,
			)
			return 0
		}

		sourceFound = true
		break
	}
	if !sourceFound {
		logger.Debug(
			"Group message committed without realtime delivery: sender presence disappeared group_id=%s message_id=%s",
			envelope.GroupID, envelope.MessageID,
		)
		return 0
	}

	targets := make([]runtime.Presence, 0)
	accessByUser := map[string]bool{
		userID: true,
	}

	for _, presence := range presences {
		targetUserID := presence.GetUserId()

		if _, isMember := memberIDs[targetUserID]; !isMember {
			continue
		}
		if targetUserID == userID &&
			presence.GetSessionId() == sessionID {
			continue
		}
		if _, metadataErr := decodeMetadata(presence.GetStatus()); metadataErr != nil {
			continue
		}

		accessAllowed, checked := accessByUser[targetUserID]
		if !checked {
			accessObject, recipientAccess, accessErr := readGroupAccess(
				ctx, nk, envelope.GroupID, targetUserID,
			)
			accessAllowed = accessErr == nil &&
				accessObject != nil &&
				recipientAccess.JoinedAt != "" &&
				recipientAccess.RevokedAt == ""

			accessByUser[targetUserID] = accessAllowed

			if accessErr != nil {
				logger.Debug(
					"Skipping group recipient: access lookup failed group_id=%s message_id=%s user_id=%s error=%v",
					envelope.GroupID, envelope.MessageID,
					targetUserID, accessErr,
				)
			}
		}

		if !accessAllowed {
			continue
		}

		targets = append(targets, presence)
	}

	if len(targets) == 0 {
		return 0
	}

	relayPayload, err := encodeAgentRelay(userID, sessionID, payload)
	if err != nil {
		logger.Debug(
			"Group message committed without realtime delivery: relay encoding failed group_id=%s message_id=%s error=%v",
			envelope.GroupID, envelope.MessageID, err,
		)
		return 0
	}

	if err := nk.StreamSend(
		agentStreamMode,
		"",
		"",
		agentStreamLabel,
		relayPayload,
		targets,
		true,
	); err != nil {
		logger.Debug(
			"Group message committed without realtime delivery: stream send failed group_id=%s message_id=%s recipients=%d error=%v",
			envelope.GroupID, envelope.MessageID, len(targets), err,
		)
		return 0
	}

	return len(targets)
}

func groupSend(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, sessionID, err := authenticatedSession(ctx)
	if err != nil {
		return "", err
	}

	envelope, err := validateGroupEnvelope(payload)
	if err != nil {
		return "", fmt.Errorf("invalid group message: %w", err)
	}
	if err := validateGroupID(envelope.GroupID); err != nil {
		return "", err
	}

	group, err := loadGroup(ctx, nk, envelope.GroupID)
	if err != nil {
		return "", err
	}
	if err := ensureGroupActive(group); err != nil {
		return "", err
	}

	member, role, _, err := groupMembership(
		ctx, nk, envelope.GroupID, userID,
	)
	if err != nil {
		return "", err
	}
	if !member || (role != "SUPERADMIN" &&
		role != "ADMIN" && role != "MEMBER") {
		return "", errors.New("active group membership required")
	}

	_, access, err := readGroupAccess(
		ctx, nk, envelope.GroupID, userID,
	)
	if err != nil {
		return "", err
	}
	if access.JoinedAt == "" || access.RevokedAt != "" {
		return "", errors.New("group history access is inactive")
	}

	presences, err := listAgentPresences(nk)
	if err != nil {
		return "", err
	}

	sourceFound := false
	for _, presence := range presences {
		if presence.GetUserId() != userID ||
			presence.GetSessionId() != sessionID {
			continue
		}

		metadata, metadataErr := decodeMetadata(presence.GetStatus())
		if metadataErr != nil {
			return "", fmt.Errorf(
				"invalid authenticated agent presence: %w",
				metadataErr,
			)
		}
		if metadata.AgentID != envelope.Source.AgentID ||
			metadata.InstanceID != envelope.Source.InstanceID {
			return "", errors.New(
				"message source does not match authenticated agent",
			)
		}

		sourceFound = true
		break
	}
	if !sourceFound {
		return "", errors.New(
			"authenticated session has no active agent registration",
		)
	}

	contentHash, err := groupMessageContentHash(userID, envelope)
	if err != nil {
		return "", fmt.Errorf("hash group message: %w", err)
	}

	index, created, err := reserveGroupMessage(
		ctx, nk, envelope.GroupID, userID, envelope, contentHash,
	)
	if err != nil {
		return "", err
	}

	if err := ensureGroupMessageStored(
		ctx, nk, index, userID, envelope,
	); err != nil {
		return "", err
	}

	if err := commitGroupMessageIndex(ctx, nk, index); err != nil {
		return "", err
	}

	recipients := 0
	if created {
		recipients = deliverGroupMessageBestEffort(
			ctx,
			logger,
			nk,
			userID,
			sessionID,
			payload,
			envelope,
		)
	}

	logger.Debug(
		"Group message committed: group_id=%s source=%s message_id=%s new=%t recipients=%d",
		envelope.GroupID,
		envelope.Source.AgentID,
		envelope.MessageID,
		created,
		recipients,
	)

	data, err := json.Marshal(successResponse{Success: true})
	if err != nil {
		return "", fmt.Errorf("encode group send response: %w", err)
	}
	return string(data), nil
}

// Nakama automatically removes the session's stream presence when the
// session ends. This callback only records the lifecycle event.
func cleanupSession(
	ctx context.Context,
	logger runtime.Logger,
	evt *api.Event,
) {
	userID, _ := ctx.Value(runtime.RUNTIME_CTX_USER_ID).(string)
	sessionID, _ := ctx.Value(runtime.RUNTIME_CTX_SESSION_ID).(string)

	logger.Info(
		"Agent session ended: user_id=%s session_id=%s",
		userID,
		sessionID,
	)
}

// Group access records are system-owned. Native Nakama membership remains
// authoritative; these records track when a user may access group history.
const (
	groupAccessCollection       = "mcp_group_access"
	groupMessageIndexCollection = "mcp_group_message_index"
	systemStorageUserID         = "00000000-0000-0000-0000-000000000000"
	maxGroupNameSize            = 64
	maxGroupDescription         = 1024
	maxGroupMembers             = 1000
	groupListLimit              = 100
)

type groupCreateRequest struct {
	Name        string `json:"name"`
	Description string `json:"description"`
	LangTag     string `json:"lang_tag"`
	AvatarURL   string `json:"avatar_url"`
	Open        bool   `json:"open"`
	MaxCount    int    `json:"max_count"`
}

type groupDiscoverRequest struct {
	Name    string `json:"name"`
	LangTag string `json:"lang_tag"`
	Limit   int    `json:"limit"`
	Cursor  string `json:"cursor"`
}

type groupIDRequest struct {
	GroupID string `json:"group_id"`
}

type groupSummary struct {
	ID          string `json:"id"`
	CreatorID   string `json:"creator_id"`
	Name        string `json:"name"`
	Description string `json:"description"`
	LangTag     string `json:"lang_tag"`
	Open        bool   `json:"open"`
	EdgeCount   int32  `json:"edge_count"`
	MaxCount    int32  `json:"max_count"`
}

type groupDiscoverResponse struct {
	Groups []groupSummary `json:"groups"`
	Cursor string         `json:"cursor,omitempty"`
}

type groupGetResponse struct {
	Group    groupSummary `json:"group"`
	IsMember bool         `json:"is_member"`
	Role     string       `json:"role,omitempty"`
}

type groupAccessRecord struct {
	GroupID   string `json:"group_id"`
	UserID    string `json:"user_id"`
	JoinedAt  string `json:"joined_at"`
	RevokedAt string `json:"revoked_at,omitempty"`
}

type groupMessageRecord struct {
	GroupID     string          `json:"group_id"`
	MessageID   string          `json:"message_id"`
	UserID      string          `json:"user_id"`
	ContentHash string          `json:"content_hash"`
	Envelope    json.RawMessage `json:"envelope"`
	StoredAt    string          `json:"stored_at"`
}

type groupMessageIndex struct {
	GroupID     string `json:"group_id"`
	MessageID   string `json:"message_id"`
	ContentHash string `json:"content_hash"`
	MessageKey  string `json:"message_key"`
	Timestamp   string `json:"timestamp"`
	State       string `json:"state"`
}

type groupHistoryRequest struct {
	GroupID string `json:"group_id"`
	Cursor  string `json:"cursor,omitempty"`
	Limit   int    `json:"limit,omitempty"`
	Since   string `json:"since,omitempty"`
	Until   string `json:"until,omitempty"`
}

type groupHistoryResponse struct {
	Messages []groupMessageRecord `json:"messages"`
	Cursor   string               `json:"cursor,omitempty"`
	HasMore  bool                 `json:"has_more"`
}

type groupHistoryCursor struct {
	Version         int      `json:"version"`
	GroupID         string   `json:"group_id"`
	UserID          string   `json:"user_id"`
	Since           string   `json:"since"`
	Until           string   `json:"until"`
	StorageCursor   string   `json:"storage_cursor"`
	StorageComplete bool     `json:"storage_complete,omitempty"`
	PendingKeys     []string `json:"pending_keys,omitempty"`
}

func groupMessageIndexKey(messageID string) string {
	sum := sha256.Sum256([]byte(messageID))
	return hex.EncodeToString(sum[:])
}

func canonicalizeGroupJSON(raw json.RawMessage) (json.RawMessage, error) {
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()

	var value interface{}
	if err := decoder.Decode(&value); err != nil {
		return nil, fmt.Errorf("decode JSON for canonicalization: %w", err)
	}

	var extra interface{}
	if err := decoder.Decode(&extra); err != io.EOF {
		return nil, errors.New("unexpected trailing JSON during canonicalization")
	}

	normalized, err := json.Marshal(value)
	if err != nil {
		return nil, fmt.Errorf("canonicalize JSON: %w", err)
	}
	return json.RawMessage(normalized), nil
}

func groupMessageContentHash(userID string, envelope agentEnvelope) (string, error) {
	canonicalPayload, err := canonicalizeGroupJSON(envelope.Payload)
	if err != nil {
		return "", err
	}

	normalizedEnvelope := envelope
	normalizedEnvelope.Payload = canonicalPayload

	normalized, err := json.Marshal(normalizedEnvelope)
	if err != nil {
		return "", fmt.Errorf("normalize group message envelope: %w", err)
	}

	content, err := json.Marshal(struct {
		UserID   string          `json:"user_id"`
		Envelope json.RawMessage `json:"envelope"`
	}{
		UserID:   userID,
		Envelope: normalized,
	})
	if err != nil {
		return "", fmt.Errorf("encode group message hash input: %w", err)
	}

	sum := sha256.Sum256(content)
	return hex.EncodeToString(sum[:]), nil
}

const groupMessageKeyTimeLayout = "20060102T150405.000000000Z"

func validGroupMessageKey(key string) bool {
	const timestampLength = len(groupMessageKeyTimeLayout)

	if len(key) != timestampLength+1+sha256.Size*2 ||
		key[timestampLength] != '_' {
		return false
	}

	timestamp, err := time.Parse(groupMessageKeyTimeLayout, key[:timestampLength])
	if err != nil || timestamp.UTC().Format(groupMessageKeyTimeLayout) != key[:timestampLength] {
		return false
	}

	digest, err := hex.DecodeString(key[timestampLength+1:])
	return err == nil &&
		len(digest) == sha256.Size &&
		hex.EncodeToString(digest) == key[timestampLength+1:]
}

func groupMessageCollection(groupID string) string {
	return "mcp_g_" + groupID
}

func readGroupMessageIndex(
	ctx context.Context,
	nk runtime.NakamaModule,
	messageID string,
) (*api.StorageObject, groupMessageIndex, error) {
	objects, err := nk.StorageRead(ctx, []*runtime.StorageRead{{
		Collection: groupMessageIndexCollection,
		Key:        groupMessageIndexKey(messageID),
		UserID:     systemStorageUserID,
	}})
	if err != nil {
		return nil, groupMessageIndex{}, fmt.Errorf("read group message index: %w", err)
	}
	if len(objects) == 0 || objects[0] == nil {
		return nil, groupMessageIndex{}, nil
	}
	if len(objects) != 1 {
		return nil, groupMessageIndex{}, errors.New("multiple group message indexes returned")
	}

	object := objects[0]
	var index groupMessageIndex
	if err := decodeJSON(object.GetValue(), &index); err != nil {
		return nil, groupMessageIndex{}, fmt.Errorf("decode group message index: %w", err)
	}
	if index.MessageID != messageID ||
		index.GroupID == "" ||
		index.ContentHash == "" ||
		index.MessageKey == "" ||
		index.Timestamp == "" ||
		(index.State != "PENDING" && index.State != "COMMITTED") {
		return nil, groupMessageIndex{}, errors.New("group message index is invalid")
	}
	return object, index, nil
}

// reserveGroupMessage creates a globally unique message-ID reservation.
// created is true only for the call that successfully created the reservation.
func reserveGroupMessage(
	ctx context.Context,
	nk runtime.NakamaModule,
	groupID, userID string,
	envelope agentEnvelope,
	contentHash string,
) (groupMessageIndex, bool, error) {
	if groupID == "" || userID == "" || envelope.MessageID == "" || contentHash == "" {
		return groupMessageIndex{}, false, errors.New("incomplete group message reservation")
	}

	now := time.Now().UTC()
	timestamp := now.Format(time.RFC3339Nano)
	messageKey := now.Format(groupMessageKeyTimeLayout) + "_" +
		groupMessageIndexKey(envelope.MessageID)

	candidate := groupMessageIndex{
		GroupID:     groupID,
		MessageID:   envelope.MessageID,
		ContentHash: contentHash,
		MessageKey:  messageKey,
		Timestamp:   timestamp,
		State:       "PENDING",
	}

	createdByThisCall := false
	read := &runtime.StorageRead{
		Collection: groupMessageIndexCollection,
		Key:        groupMessageIndexKey(envelope.MessageID),
		UserID:     systemStorageUserID,
	}

	_, err := nk.StorageWriteRetry(
		ctx,
		[]*runtime.StorageRead{read},
		func(objects []*api.StorageObject) ([]*runtime.StorageWrite, error) {
			createdByThisCall = false

			if len(objects) > 1 {
				return nil, errors.New("multiple group message indexes returned")
			}

			if len(objects) == 1 && objects[0] != nil {
				var existing groupMessageIndex
				if err := decodeJSON(objects[0].GetValue(), &existing); err != nil {
					return nil, fmt.Errorf("decode existing group message index: %w", err)
				}
				if existing.MessageID != envelope.MessageID {
					return nil, errors.New("group message index key collision")
				}
				if existing.GroupID != groupID ||
					existing.ContentHash != contentHash {
					return nil, errors.New("message_id already used with different content")
				}
				if existing.MessageKey == "" ||
					existing.Timestamp == "" ||
					(existing.State != "PENDING" && existing.State != "COMMITTED") {
					return nil, errors.New("existing group message index is invalid")
				}
				return nil, nil
			}

			value, err := json.Marshal(candidate)
			if err != nil {
				return nil, fmt.Errorf("encode group message reservation: %w", err)
			}

			createdByThisCall = true
			return []*runtime.StorageWrite{{
				Collection:      groupMessageIndexCollection,
				Key:             groupMessageIndexKey(envelope.MessageID),
				UserID:          systemStorageUserID,
				Value:           string(value),
				Version:         "*",
				PermissionRead:  0,
				PermissionWrite: 0,
			}}, nil
		},
		5,
	)
	if err != nil {
		return groupMessageIndex{}, false, fmt.Errorf("reserve group message ID: %w", err)
	}

	_, index, err := readGroupMessageIndex(ctx, nk, envelope.MessageID)
	if err != nil {
		return groupMessageIndex{}, false, err
	}
	if index.MessageID == "" {
		return groupMessageIndex{}, false, errors.New("group message reservation was not persisted")
	}
	if index.GroupID != groupID || index.ContentHash != contentHash {
		return groupMessageIndex{}, false, errors.New("message_id already used with different content")
	}

	return index, createdByThisCall, nil
}

func ensureGroupMessageStored(
	ctx context.Context,
	nk runtime.NakamaModule,
	index groupMessageIndex,
	userID string,
	envelope agentEnvelope,
) error {
	if index.GroupID == "" || index.MessageID == "" ||
		index.MessageKey == "" || index.Timestamp == "" ||
		index.ContentHash == "" || userID == "" {
		return errors.New("incomplete group message storage record")
	}
	if envelope.GroupID != index.GroupID ||
		envelope.MessageID != index.MessageID {
		return errors.New("group message envelope does not match its reservation")
	}

	expectedHash, err := groupMessageContentHash(userID, envelope)
	if err != nil {
		return fmt.Errorf("hash group message before storage: %w", err)
	}
	if expectedHash != index.ContentHash {
		return errors.New("group message content does not match its reservation")
	}

	envelopeJSON, err := json.Marshal(envelope)
	if err != nil {
		return fmt.Errorf("encode group message envelope: %w", err)
	}

	expected := groupMessageRecord{
		GroupID:     index.GroupID,
		MessageID:   index.MessageID,
		UserID:      userID,
		ContentHash: index.ContentHash,
		Envelope:    envelopeJSON,
		StoredAt:    index.Timestamp,
	}

	read := func() (*api.StorageObject, error) {
		objects, err := nk.StorageRead(ctx, []*runtime.StorageRead{{
			Collection: groupMessageCollection(index.GroupID),
			Key:        index.MessageKey,
			UserID:     systemStorageUserID,
		}})
		if err != nil {
			return nil, fmt.Errorf("read historical group message: %w", err)
		}
		if len(objects) > 1 {
			return nil, errors.New("multiple historical group messages returned")
		}
		if len(objects) == 0 {
			return nil, nil
		}
		return objects[0], nil
	}

	verify := func(object *api.StorageObject) error {
		if object == nil {
			return errors.New("historical group message is missing")
		}

		var stored groupMessageRecord
		if err := decodeJSON(object.GetValue(), &stored); err != nil {
			return fmt.Errorf("decode historical group message: %w", err)
		}

		if stored.GroupID != expected.GroupID ||
			stored.MessageID != expected.MessageID ||
			stored.UserID != expected.UserID ||
			stored.ContentHash != expected.ContentHash ||
			stored.StoredAt != expected.StoredAt {
			return errors.New("historical group message does not match its reservation")
		}

		var storedEnvelope agentEnvelope
		if err := decodeJSON(string(stored.Envelope), &storedEnvelope); err != nil {
			return fmt.Errorf("decode stored group message envelope: %w", err)
		}
		if storedEnvelope.GroupID != stored.GroupID ||
			storedEnvelope.MessageID != stored.MessageID {
			return errors.New("stored envelope identity does not match its message record")
		}

		storedHash, err := groupMessageContentHash(stored.UserID, storedEnvelope)
		if err != nil {
			return fmt.Errorf("hash stored group message: %w", err)
		}
		if storedHash != stored.ContentHash {
			return errors.New("stored group message content hash mismatch")
		}
		if storedHash != expected.ContentHash {
			return errors.New("stored group message differs from the requested content")
		}
		return nil
	}

	existing, err := read()
	if err != nil {
		return err
	}
	if existing != nil {
		return verify(existing)
	}

	value, err := json.Marshal(expected)
	if err != nil {
		return fmt.Errorf("encode historical group message: %w", err)
	}

	_, writeErr := nk.StorageWrite(ctx, []*runtime.StorageWrite{{
		Collection:      groupMessageCollection(index.GroupID),
		Key:             index.MessageKey,
		UserID:          systemStorageUserID,
		Value:           string(value),
		Version:         "*",
		PermissionRead:  0,
		PermissionWrite: 0,
	}})
	if writeErr == nil {
		return nil
	}

	// A concurrent retry may have created the same immutable record.
	existing, readErr := read()
	if readErr != nil {
		return fmt.Errorf("write historical group message: %w; verify after conflict: %v", writeErr, readErr)
	}
	if existing == nil {
		return fmt.Errorf("write historical group message: %w", writeErr)
	}
	if err := verify(existing); err != nil {
		return fmt.Errorf("write historical group message: %w; existing record: %v", writeErr, err)
	}
	return nil
}

func commitGroupMessageIndex(
	ctx context.Context,
	nk runtime.NakamaModule,
	expected groupMessageIndex,
) error {
	read := &runtime.StorageRead{
		Collection: groupMessageIndexCollection,
		Key:        groupMessageIndexKey(expected.MessageID),
		UserID:     systemStorageUserID,
	}

	_, err := nk.StorageWriteRetry(
		ctx,
		[]*runtime.StorageRead{read},
		func(objects []*api.StorageObject) ([]*runtime.StorageWrite, error) {
			if len(objects) == 0 || objects[0] == nil {
				return nil, errors.New("group message reservation disappeared")
			}
			if len(objects) != 1 {
				return nil, errors.New("multiple group message indexes returned")
			}

			var current groupMessageIndex
			if err := decodeJSON(objects[0].GetValue(), &current); err != nil {
				return nil, fmt.Errorf("decode group message index before commit: %w", err)
			}
			if current.MessageID != expected.MessageID ||
				current.GroupID != expected.GroupID ||
				current.ContentHash != expected.ContentHash ||
				current.MessageKey != expected.MessageKey {
				return nil, errors.New("group message reservation changed before commit")
			}
			if current.State == "COMMITTED" {
				return nil, nil
			}
			if current.State != "PENDING" {
				return nil, errors.New("group message reservation has an invalid state")
			}

			current.State = "COMMITTED"
			value, err := json.Marshal(current)
			if err != nil {
				return nil, fmt.Errorf("encode committed group message index: %w", err)
			}

			version := objects[0].GetVersion()
			if version == "" {
				return nil, errors.New("group message index has no storage version")
			}

			return []*runtime.StorageWrite{{
				Collection:      groupMessageIndexCollection,
				Key:             groupMessageIndexKey(expected.MessageID),
				UserID:          systemStorageUserID,
				Value:           string(value),
				Version:         version,
				PermissionRead:  0,
				PermissionWrite: 0,
			}}, nil
		},
		5,
	)
	if err != nil {
		return fmt.Errorf("commit group message index: %w", err)
	}

	_, committed, err := readGroupMessageIndex(ctx, nk, expected.MessageID)
	if err != nil {
		return err
	}
	if committed.State != "COMMITTED" ||
		committed.GroupID != expected.GroupID ||
		committed.ContentHash != expected.ContentHash {
		return errors.New("group message index did not reach COMMITTED state")
	}
	return nil
}

func groupAccessKey(groupID, userID string) string {
	return "g_" + groupID + "_u_" + userID
}

func groupSummaryFromAPI(group *api.Group) groupSummary {
	result := groupSummary{
		ID:          group.GetId(),
		CreatorID:   group.GetCreatorId(),
		Name:        group.GetName(),
		Description: group.GetDescription(),
		LangTag:     group.GetLangTag(),
		EdgeCount:   group.GetEdgeCount(),
		MaxCount:    group.GetMaxCount(),
	}
	if group.GetOpen() != nil {
		result.Open = group.GetOpen().GetValue()
	}
	return result
}

func authenticatedGroupIdentity(ctx context.Context) (string, string, error) {
	userID, _, err := authenticatedSession(ctx)
	if err != nil {
		return "", "", err
	}

	username, ok := ctx.Value(runtime.RUNTIME_CTX_USERNAME).(string)
	if !ok || username == "" {
		return "", "", errors.New("authenticated Nakama username is unavailable")
	}
	return userID, username, nil
}

func validateGroupID(groupID string) error {
	if !validIdentifier(groupID) {
		return errors.New("invalid group_id")
	}
	return nil
}

func readGroupAccess(
	ctx context.Context,
	nk runtime.NakamaModule,
	groupID, userID string,
) (*api.StorageObject, groupAccessRecord, error) {
	objects, err := nk.StorageRead(ctx, []*runtime.StorageRead{{
		Collection: groupAccessCollection,
		Key:        groupAccessKey(groupID, userID),
		UserID:     systemStorageUserID,
	}})
	if err != nil {
		return nil, groupAccessRecord{}, fmt.Errorf("read group access record: %w", err)
	}
	if len(objects) > 1 {
		return nil, groupAccessRecord{}, errors.New(
			"multiple group access records returned",
		)
	}
	if len(objects) == 0 || objects[0] == nil {
		return nil, groupAccessRecord{}, nil
	}

	object := objects[0]
	var record groupAccessRecord
	if err := decodeJSON(object.GetValue(), &record); err != nil {
		return nil, groupAccessRecord{}, fmt.Errorf("decode group access record: %w", err)
	}
	if record.GroupID != groupID || record.UserID != userID || record.JoinedAt == "" {
		return nil, groupAccessRecord{}, errors.New("group access record identity or data is invalid")
	}
	return object, record, nil
}

func writeGroupAccess(
	ctx context.Context,
	nk runtime.NakamaModule,
	groupID, userID string,
	revoke bool,
) error {
	read := &runtime.StorageRead{
		Collection: groupAccessCollection,
		Key:        groupAccessKey(groupID, userID),
		UserID:     systemStorageUserID,
	}

	_, err := nk.StorageWriteRetry(
		ctx,
		[]*runtime.StorageRead{read},
		func(objects []*api.StorageObject) ([]*runtime.StorageWrite, error) {
			var object *api.StorageObject
			if len(objects) > 1 {
				return nil, errors.New("multiple group access records returned")
			}
			if len(objects) == 1 && objects[0] == nil {
				return nil, errors.New("nil group access record returned")
			}
			if len(objects) == 1 {
				object = objects[0]
			}
			var record groupAccessRecord
			if object == nil {
				if revoke {
					// No access record means there is no historical access to revoke.
					return nil, nil
				}
				record = groupAccessRecord{
					GroupID:  groupID,
					UserID:   userID,
					JoinedAt: time.Now().UTC().Format(time.RFC3339Nano),
				}
			} else {
				if err := decodeJSON(object.GetValue(), &record); err != nil {
					return nil, fmt.Errorf("decode group access record: %w", err)
				}
				if record.GroupID != groupID ||
					record.UserID != userID ||
					record.JoinedAt == "" {
					return nil, errors.New("group access record identity or data is invalid")
				}

				if revoke {
					if record.RevokedAt != "" {
						return nil, nil
					}
					record.RevokedAt = time.Now().UTC().Format(time.RFC3339Nano)
				} else {
					if record.RevokedAt == "" {
						return nil, nil
					}
					record.JoinedAt = time.Now().UTC().Format(time.RFC3339Nano)
					record.RevokedAt = ""
				}
			}

			value, err := json.Marshal(record)
			if err != nil {
				return nil, fmt.Errorf("encode group access record: %w", err)
			}

			version := "*"
			if object != nil {
				version = object.GetVersion()
				if version == "" {
					return nil, errors.New("group access record has no storage version")
				}
			}

			return []*runtime.StorageWrite{{
				Collection:      groupAccessCollection,
				Key:             groupAccessKey(groupID, userID),
				UserID:          systemStorageUserID,
				Value:           string(value),
				Version:         version,
				PermissionRead:  0,
				PermissionWrite: 0,
			}}, nil
		},
		5,
	)
	if err != nil {
		return fmt.Errorf("write group access record with retries: %w", err)
	}
	return nil
}

func groupMembership(
	ctx context.Context,
	nk runtime.NakamaModule,
	groupID, userID string,
) (bool, string, int, error) {
	cursor := ""
	superadmins := 0
	found := false
	role := ""

	for pages := 0; pages < 100; pages++ {
		users, next, err := nk.GroupUsersList(
			ctx, groupID, groupListLimit, nil, cursor,
		)
		if err != nil {
			return false, "", superadmins, fmt.Errorf("list group members: %w", err)
		}

		for _, member := range users {
			if member == nil || member.GetUser() == nil || member.GetState() == nil {
				continue
			}

			state := int(member.GetState().GetValue())
			if state == int(api.GroupUserList_GroupUser_SUPERADMIN) {
				superadmins++
			}

			if member.GetUser().GetId() != userID {
				continue
			}

			switch state {
			case int(api.GroupUserList_GroupUser_SUPERADMIN):
				found, role = true, "SUPERADMIN"
			case int(api.GroupUserList_GroupUser_ADMIN):
				found, role = true, "ADMIN"
			case int(api.GroupUserList_GroupUser_MEMBER):
				found, role = true, "MEMBER"
			case int(api.GroupUserList_GroupUser_JOIN_REQUEST):
				role = "JOIN_REQUEST"
			}
		}

		if next == "" {
			return found, role, superadmins, nil
		}
		if next == cursor {
			return false, "", superadmins, errors.New("group membership pagination cursor did not advance")
		}
		cursor = next
	}

	return false, "", superadmins, errors.New("group membership pagination limit exceeded")
}

func groupMemberUserIDs(
	ctx context.Context,
	nk runtime.NakamaModule,
	groupID string,
) (map[string]struct{}, error) {
	members := make(map[string]struct{})
	cursor := ""

	for pages := 0; pages < 100; pages++ {
		users, next, err := nk.GroupUsersList(
			ctx, groupID, groupListLimit, nil, cursor,
		)
		if err != nil {
			return nil, fmt.Errorf("list group members: %w", err)
		}

		for _, member := range users {
			if member == nil || member.GetUser() == nil ||
				member.GetState() == nil {
				continue
			}

			state := int(member.GetState().GetValue())
			switch state {
			case int(api.GroupUserList_GroupUser_SUPERADMIN),
				int(api.GroupUserList_GroupUser_ADMIN),
				int(api.GroupUserList_GroupUser_MEMBER):
				userID := member.GetUser().GetId()
				if userID != "" {
					members[userID] = struct{}{}
				}
			}
		}

		if next == "" {
			return members, nil
		}
		if next == cursor {
			return nil, errors.New(
				"group member pagination cursor did not advance",
			)
		}
		cursor = next
	}

	return nil, errors.New("group member pagination limit exceeded")
}

func loadGroup(
	ctx context.Context,
	nk runtime.NakamaModule,
	groupID string,
) (*api.Group, error) {
	groups, err := nk.GroupsGetId(ctx, []string{groupID})
	if err != nil {
		return nil, fmt.Errorf("get group: %w", err)
	}
	if len(groups) == 0 || groups[0] == nil {
		return nil, errors.New("group not found")
	}
	return groups[0], nil
}

func groupCreate(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, _, err := authenticatedSession(ctx)
	if err != nil {
		return "", err
	}

	var request groupCreateRequest
	if err := decodeJSON(payload, &request); err != nil {
		return "", err
	}
	if request.Name == "" || len(request.Name) > maxGroupNameSize ||
		strings.TrimSpace(request.Name) != request.Name {
		return "", errors.New("group name must contain 1 to 64 bytes without surrounding whitespace")
	}
	if len(request.Description) > maxGroupDescription {
		return "", errors.New("group description is too long")
	}
	if len(request.LangTag) > 35 || len(request.AvatarURL) > 2048 {
		return "", errors.New("group language tag or avatar URL is too long")
	}
	if request.MaxCount == 0 {
		request.MaxCount = 100
	}
	if request.MaxCount < 2 || request.MaxCount > maxGroupMembers {
		return "", fmt.Errorf("max_count must be between 2 and %d", maxGroupMembers)
	}

	group, err := nk.GroupCreate(
		ctx,
		userID,
		request.Name,
		userID,
		request.LangTag,
		request.Description,
		request.AvatarURL,
		request.Open,
		map[string]interface{}{"type": "USER_CREATED"},
		request.MaxCount,
	)
	if err != nil {
		return "", fmt.Errorf("create Nakama group: %w", err)
	}
	if group == nil || group.GetId() == "" {
		return "", errors.New("Nakama created a group without returning its ID")
	}

	if err := writeGroupAccess(ctx, nk, group.GetId(), userID, false); err != nil {
		if deleteErr := nk.GroupDelete(ctx, group.GetId()); deleteErr != nil {
			logger.Error(
				"Group creation compensation failed: group_id=%s error=%v",
				group.GetId(), deleteErr,
			)
		}
		return "", fmt.Errorf("initialize group access record: %w", err)
	}

	response := struct {
		Success bool         `json:"success"`
		Group   groupSummary `json:"group"`
	}{
		Success: true,
		Group:   groupSummaryFromAPI(group),
	}
	data, err := json.Marshal(response)
	if err != nil {
		return "", fmt.Errorf("encode group creation response: %w", err)
	}

	logger.Info("Group created: group_id=%s owner=%s", group.GetId(), userID)
	return string(data), nil
}

func groupDiscover(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	if _, _, err := authenticatedSession(ctx); err != nil {
		return "", err
	}

	var request groupDiscoverRequest
	if err := decodeJSON(payload, &request); err != nil {
		return "", err
	}
	if len(request.Name) > maxGroupNameSize || len(request.LangTag) > 35 {
		return "", errors.New("invalid group discovery filter")
	}
	if len(request.Cursor) > 2048 {
		return "", errors.New("group cursor is too long")
	}
	if request.Limit == 0 {
		request.Limit = 20
	}
	if request.Limit < 1 || request.Limit > groupListLimit {
		return "", fmt.Errorf("limit must be between 1 and %d", groupListLimit)
	}

	open := true
	groups, cursor, err := nk.GroupsList(
		ctx,
		request.Name,
		request.LangTag,
		nil,
		&open,
		request.Limit,
		request.Cursor,
	)
	if err != nil {
		return "", fmt.Errorf("discover Nakama groups: %w", err)
	}

	response := groupDiscoverResponse{
		Groups: make([]groupSummary, 0, len(groups)),
		Cursor: cursor,
	}
	for _, group := range groups {
		if group != nil {
			response.Groups = append(response.Groups, groupSummaryFromAPI(group))
		}
	}

	data, err := json.Marshal(response)
	if err != nil {
		return "", fmt.Errorf("encode group discovery response: %w", err)
	}
	return string(data), nil
}

func groupGet(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, _, err := authenticatedSession(ctx)
	if err != nil {
		return "", err
	}

	var request groupIDRequest
	if err := decodeJSON(payload, &request); err != nil {
		return "", err
	}
	if err := validateGroupID(request.GroupID); err != nil {
		return "", err
	}

	group, err := loadGroup(ctx, nk, request.GroupID)
	if err != nil {
		return "", err
	}

	member, role, _, err := groupMembership(ctx, nk, request.GroupID, userID)
	if err != nil {
		return "", err
	}

	if !member && (group.GetOpen() == nil || !group.GetOpen().GetValue()) {
		return "", errors.New("group not found")
	}

	if member {
		_, record, err := readGroupAccess(ctx, nk, request.GroupID, userID)
		if err != nil {
			return "", err
		}
		if record.JoinedAt == "" || record.RevokedAt != "" {
			return "", errors.New("group access record is missing or inactive")
		}
	}

	data, err := json.Marshal(groupGetResponse{
		Group:    groupSummaryFromAPI(group),
		IsMember: member,
		Role:     role,
	})
	if err != nil {
		return "", fmt.Errorf("encode group response: %w", err)
	}
	return string(data), nil
}

func groupJoin(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, username, err := authenticatedGroupIdentity(ctx)
	if err != nil {
		return "", err
	}

	var request groupIDRequest
	if err := decodeJSON(payload, &request); err != nil {
		return "", err
	}
	if err := validateGroupID(request.GroupID); err != nil {
		return "", err
	}

	group, err := loadGroup(ctx, nk, request.GroupID)
	if err != nil {
		return "", err
	}
	if err := ensureGroupActive(group); err != nil {
		return "", err
	}
	if group.GetOpen() == nil || !group.GetOpen().GetValue() {
		return "", errors.New("group is closed; an administrator must add the user")
	}

	member, role, _, err := groupMembership(ctx, nk, request.GroupID, userID)
	if err != nil {
		return "", err
	}
	if member {
		accessObject, access, err := readGroupAccess(
			ctx, nk, request.GroupID, userID,
		)
		if err != nil {
			return "", err
		}

		if accessObject == nil {
			if err := writeGroupAccess(
				ctx, nk, request.GroupID, userID, false,
			); err != nil {
				return "", err
			}
		} else if access.RevokedAt != "" {
			return "", errors.New(
				"group membership is active but historical access is revoked; leave or request administrator reconciliation",
			)
		}

		data, err := json.Marshal(groupGetResponse{
			Group:    groupSummaryFromAPI(group),
			IsMember: true,
			Role:     role,
		})
		if err != nil {
			return "", fmt.Errorf("encode group join response: %w", err)
		}
		return string(data), nil
	}

	if err := nk.GroupUserJoin(ctx, request.GroupID, userID, username); err != nil {
		return "", fmt.Errorf("join Nakama group: %w", err)
	}

	if err := writeGroupAccess(ctx, nk, request.GroupID, userID, false); err != nil {
		if leaveErr := nk.GroupUserLeave(ctx, request.GroupID, userID, username); leaveErr != nil {
			logger.Error(
				"Group join compensation failed: group_id=%s user_id=%s error=%v",
				request.GroupID, userID, leaveErr,
			)
		}
		return "", fmt.Errorf("initialize group access after join: %w", err)
	}

	logger.Info("User joined group: group_id=%s user_id=%s", request.GroupID, userID)
	data, err := json.Marshal(successResponse{Success: true})
	if err != nil {
		return "", fmt.Errorf("encode group join response: %w", err)
	}
	return string(data), nil
}

func groupAdvisoryLockKey(groupID string) int64 {
	sum := sha256.Sum256([]byte(groupID))
	return int64(binary.BigEndian.Uint64(sum[:8]))
}

func groupLeave(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, username, err := authenticatedGroupIdentity(ctx)
	if err != nil {
		return "", err
	}

	var request groupIDRequest
	if err := json.Unmarshal([]byte(payload), &request); err != nil {
		return "", errors.New("invalid group leave request")
	}
	if request.GroupID == "" {
		return "", errors.New("group_id is required")
	}

	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return "", fmt.Errorf("begin group leave transaction: %w", err)
	}
	defer tx.Rollback()

	_, err = tx.ExecContext(
		ctx,
		"SELECT pg_advisory_xact_lock($1)",
		groupAdvisoryLockKey(request.GroupID),
	)
	if err != nil {
		return "", fmt.Errorf("lock group membership: %w", err)
	}

	if _, err := loadGroup(ctx, nk, request.GroupID); err != nil {
		return "", err
	}

	member, role, superadmins, err := groupMembership(
		ctx,
		nk,
		request.GroupID,
		userID,
	)
	if err != nil {
		return "", err
	}
	if !member {
		return "", errors.New("user is not a member of this group")
	}

	if role == "SUPERADMIN" && superadmins <= 1 {
		return "", errors.New("last SUPERADMIN cannot leave an active group")
	}

	if err := writeGroupAccess(
		ctx,
		nk,
		request.GroupID,
		userID,
		true,
	); err != nil {
		return "", fmt.Errorf("revoke group history access: %w", err)
	}

	if err := nk.GroupUserLeave(
		ctx,
		request.GroupID,
		userID,
		username,
	); err != nil {
		return "", fmt.Errorf("leave group: %w", err)
	}

	if err := tx.Commit(); err != nil {
		return "", fmt.Errorf("commit group leave lock transaction: %w", err)
	}

	logger.Info(
		"User left group: group_id=%s user_id=%s",
		request.GroupID,
		userID,
	)

	data, err := json.Marshal(successResponse{Success: true})
	if err != nil {
		return "", fmt.Errorf("encode group leave response: %w", err)
	}
	return string(data), nil
}

type groupTargetRequest struct {
	GroupID string `json:"group_id"`
	UserID  string `json:"user_id"`
}

func groupMetadata(group *api.Group) (map[string]interface{}, error) {
	metadata := make(map[string]interface{})
	if raw := group.GetMetadata(); raw != "" {
		if err := json.Unmarshal([]byte(raw), &metadata); err != nil {
			return nil, fmt.Errorf("decode group metadata: %w", err)
		}
	}
	if metadata == nil {
		metadata = make(map[string]interface{})
	}
	return metadata, nil
}

func ensureGroupActive(group *api.Group) error {
	metadata, err := groupMetadata(group)
	if err != nil {
		return err
	}

	status, exists := metadata["status"]
	if !exists {
		return nil
	}

	value, ok := status.(string)
	if !ok {
		return errors.New("group status metadata is invalid")
	}
	switch value {
	case "", "ACTIVE":
		return nil
	case "FINISHED":
		return errors.New("group is finished")
	default:
		return errors.New("group has an unsupported status")
	}
}

func requireGroupAdmin(
	ctx context.Context,
	nk runtime.NakamaModule,
	groupID, userID string,
) (string, error) {
	member, role, _, err := groupMembership(ctx, nk, groupID, userID)
	if err != nil {
		return "", err
	}
	if !member || (role != "SUPERADMIN" && role != "ADMIN") {
		return "", errors.New("group administrator role required")
	}

	_, access, err := readGroupAccess(ctx, nk, groupID, userID)
	if err != nil {
		return "", err
	}
	if access.JoinedAt == "" || access.RevokedAt != "" {
		return "", errors.New("group administrator history access is inactive")
	}

	return role, nil
}

func groupInvite(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, _, err := authenticatedSession(ctx)
	if err != nil {
		return "", err
	}

	var request groupTargetRequest
	if err := decodeJSON(payload, &request); err != nil {
		return "", err
	}
	if err := validateGroupID(request.GroupID); err != nil {
		return "", err
	}
	if !validIdentifier(request.UserID) {
		return "", errors.New("invalid target user_id")
	}
	if request.UserID == userID {
		return "", errors.New("user is already a group administrator")
	}

	group, err := loadGroup(ctx, nk, request.GroupID)
	if err != nil {
		return "", err
	}
	if err := ensureGroupActive(group); err != nil {
		return "", err
	}

	if _, err := requireGroupAdmin(ctx, nk, request.GroupID, userID); err != nil {
		return "", err
	}

	account, err := nk.AccountGetId(ctx, request.UserID)
	if err != nil {
		return "", fmt.Errorf("look up invited Nakama account: %w", err)
	}
	if account == nil || account.GetUser() == nil {
		return "", errors.New("invited Nakama user not found")
	}

	member, targetRole, _, err := groupMembership(ctx, nk, request.GroupID, request.UserID)
	if err != nil {
		return "", err
	}
	if member {
		return "", fmt.Errorf("user is already a group member with role %s", targetRole)
	}

	if _, err := requireGroupAdmin(ctx, nk, request.GroupID, userID); err != nil {
		return "", err
	}

	group, err = loadGroup(ctx, nk, request.GroupID)
	if err != nil {
		return "", err
	}
	if err := ensureGroupActive(group); err != nil {
		return "", err
	}

	if int(group.GetEdgeCount()) >= int(group.GetMaxCount()) {
		return "", errors.New("group has reached its member limit")
	}

	if err := nk.GroupUsersAdd(ctx, userID, request.GroupID, []string{request.UserID}); err != nil {
		return "", fmt.Errorf("add user to Nakama group: %w", err)
	}

	if err := writeGroupAccess(ctx, nk, request.GroupID, request.UserID, false); err != nil {
		if kickErr := nk.GroupUsersKick(ctx, userID, request.GroupID, []string{request.UserID}); kickErr != nil {
			logger.Error(
				"Group invite compensation failed: group_id=%s user_id=%s error=%v",
				request.GroupID, request.UserID, kickErr,
			)
		}
		return "", fmt.Errorf("initialize invited user's history access: %w", err)
	}

	logger.Info("User added to group: group_id=%s actor=%s user_id=%s",
		request.GroupID, userID, request.UserID)

	data, err := json.Marshal(successResponse{Success: true})
	if err != nil {
		return "", fmt.Errorf("encode group invite response: %w", err)
	}
	return string(data), nil
}

func groupKick(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, _, err := authenticatedSession(ctx)
	if err != nil {
		return "", err
	}

	var request groupTargetRequest
	if err := decodeJSON(payload, &request); err != nil {
		return "", err
	}
	if err := validateGroupID(request.GroupID); err != nil {
		return "", err
	}
	if !validIdentifier(request.UserID) {
		return "", errors.New("invalid target user_id")
	}
	if request.UserID == userID {
		return "", errors.New("use group_leave to leave the group")
	}

	group, err := loadGroup(ctx, nk, request.GroupID)
	if err != nil {
		return "", err
	}
	if err := ensureGroupActive(group); err != nil {
		return "", err
	}

	actorRole, err := requireGroupAdmin(ctx, nk, request.GroupID, userID)
	if err != nil {
		return "", err
	}

	targetMember, targetRole, _, err := groupMembership(
		ctx, nk, request.GroupID, request.UserID,
	)
	if err != nil {
		return "", err
	}
	if !targetMember {
		return "", errors.New("target is not an active group member")
	}
	if targetRole == "SUPERADMIN" {
		return "", errors.New("SUPERADMIN cannot be kicked")
	}
	if actorRole == "ADMIN" && targetRole != "MEMBER" {
		return "", errors.New("ADMIN can only kick regular members")
	}

	accessObject, access, err := readGroupAccess(
		ctx, nk, request.GroupID, request.UserID,
	)
	if err != nil {
		return "", err
	}
	if accessObject != nil && access.RevokedAt == "" {
		if err := writeGroupAccess(
			ctx, nk, request.GroupID, request.UserID, true,
		); err != nil {
			return "", fmt.Errorf("revoke target history access: %w", err)
		}
	}

	// Revoke history access before changing native group membership.
	if err := nk.GroupUsersKick(
		ctx, userID, request.GroupID, []string{request.UserID},
	); err != nil {
		return "", fmt.Errorf("kick user from Nakama group: %w", err)
	}

	logger.Info(
		"User kicked from group: group_id=%s actor=%s user_id=%s",
		request.GroupID, userID, request.UserID,
	)

	data, err := json.Marshal(successResponse{Success: true})
	if err != nil {
		return "", fmt.Errorf("encode group kick response: %w", err)
	}
	return string(data), nil
}

func normalizeHistoryTime(value string) (string, time.Time, error) {
	if value == "" {
		return "", time.Time{}, nil
	}

	parsed, err := time.Parse(time.RFC3339Nano, value)
	if err != nil {
		return "", time.Time{}, errors.New("history timestamps must use RFC3339")
	}

	parsed = parsed.UTC()
	return parsed.Format(time.RFC3339Nano), parsed, nil
}

func encodeGroupHistoryCursor(cursor groupHistoryCursor) (string, error) {
	data, err := json.Marshal(cursor)
	if err != nil {
		return "", fmt.Errorf("encode group history cursor: %w", err)
	}

	encoded := hex.EncodeToString(data)
	if len(encoded) > groupHistoryMaxCursorSize {
		return "", errors.New("group history cursor is too large")
	}
	return encoded, nil
}

func decodeGroupHistoryCursor(value string) (groupHistoryCursor, error) {
	if len(value) == 0 || len(value) > groupHistoryMaxCursorSize {
		return groupHistoryCursor{}, errors.New("invalid group history cursor size")
	}

	data, err := hex.DecodeString(value)
	if err != nil {
		return groupHistoryCursor{}, errors.New("invalid group history cursor encoding")
	}

	var cursor groupHistoryCursor
	if err := decodeJSON(string(data), &cursor); err != nil {
		return groupHistoryCursor{}, fmt.Errorf("invalid group history cursor: %w", err)
	}

	if cursor.Version != 1 ||
		cursor.GroupID == "" ||
		cursor.UserID == "" ||
		len(cursor.PendingKeys) > groupHistoryPageSize {
		return groupHistoryCursor{}, errors.New("invalid group history cursor data")
	}

	for _, key := range cursor.PendingKeys {
		if !validGroupMessageKey(key) {
			return groupHistoryCursor{}, errors.New("invalid pending history key")
		}
	}

	return cursor, nil
}

var errGroupHistoryMessagePending = errors.New("group history message is pending")

func verifyGroupHistoryObject(
	ctx context.Context,
	nk runtime.NakamaModule,
	groupID string,
	object *api.StorageObject,
) (groupMessageRecord, time.Time, error) {
	if object == nil ||
		object.GetCollection() != groupMessageCollection(groupID) ||
		object.GetUserId() != systemStorageUserID ||
		!validGroupMessageKey(object.GetKey()) {
		return groupMessageRecord{}, time.Time{}, errors.New("invalid group history storage object")
	}

	var record groupMessageRecord
	if err := decodeJSON(object.GetValue(), &record); err != nil {
		return record, time.Time{}, fmt.Errorf("decode group history message: %w", err)
	}

	if record.GroupID != groupID ||
		record.MessageID == "" ||
		record.UserID == "" ||
		record.ContentHash == "" ||
		len(record.Envelope) == 0 ||
		record.StoredAt == "" {
		return record, time.Time{}, errors.New("group history message has invalid fields")
	}

	storedAt, err := time.Parse(time.RFC3339Nano, record.StoredAt)
	if err != nil {
		return record, time.Time{}, errors.New("group history message has invalid timestamp")
	}
	storedAt = storedAt.UTC()

	_, index, err := readGroupMessageIndex(ctx, nk, record.MessageID)
	if err != nil {
		return record, time.Time{}, err
	}
	if index.MessageID == "" {
		return record, time.Time{}, errors.New("group history message index is missing")
	}
	if index.State != "PENDING" && index.State != "COMMITTED" {
		return record, time.Time{}, errors.New("group history message has an invalid index state")
	}
	if index.GroupID != groupID ||
		index.MessageKey != object.GetKey() ||
		index.ContentHash != record.ContentHash ||
		index.Timestamp != record.StoredAt {
		return record, time.Time{}, errors.New("group history message does not match its index")
	}

	var envelope agentEnvelope
	if err := decodeJSON(string(record.Envelope), &envelope); err != nil {
		return record, time.Time{}, fmt.Errorf("decode group history envelope: %w", err)
	}
	if _, err := validateGroupEnvelope(string(record.Envelope)); err != nil {
		return record, time.Time{}, fmt.Errorf("invalid group history envelope: %w", err)
	}
	if envelope.GroupID != groupID || envelope.MessageID != record.MessageID {
		return record, time.Time{}, errors.New("group history envelope identity mismatch")
	}

	expectedHash, err := groupMessageContentHash(record.UserID, envelope)
	if err != nil {
		return record, time.Time{}, fmt.Errorf("verify group history content hash: %w", err)
	}
	if expectedHash != record.ContentHash {
		return record, time.Time{}, errors.New("group history content hash mismatch")
	}

	if index.State == "PENDING" {
		return record, storedAt, errGroupHistoryMessagePending
	}

	return record, storedAt, nil
}

func readGroupHistoryObject(
	ctx context.Context,
	nk runtime.NakamaModule,
	groupID, key string,
) (*api.StorageObject, error) {
	objects, err := nk.StorageRead(ctx, []*runtime.StorageRead{{
		Collection: groupMessageCollection(groupID),
		Key:        key,
		UserID:     systemStorageUserID,
	}})
	if err != nil {
		return nil, fmt.Errorf("read pending group history message: %w", err)
	}
	if len(objects) != 1 || objects[0] == nil {
		return nil, errors.New("pending group history message disappeared")
	}
	return objects[0], nil
}

func groupHistory(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, _, err := authenticatedSession(ctx)
	if err != nil {
		return "", err
	}

	var request groupHistoryRequest
	if err := decodeJSON(payload, &request); err != nil {
		return "", err
	}
	if err := validateGroupID(request.GroupID); err != nil {
		return "", err
	}

	if request.Limit == 0 {
		request.Limit = 50
	}
	if request.Limit < 1 || request.Limit > groupHistoryPageSize {
		return "", fmt.Errorf("limit must be between 1 and %d", groupHistoryPageSize)
	}

	sinceValue, sinceTime, err := normalizeHistoryTime(request.Since)
	if err != nil {
		return "", fmt.Errorf("invalid since: %w", err)
	}
	untilValue, untilTime, err := normalizeHistoryTime(request.Until)
	if err != nil {
		return "", fmt.Errorf("invalid until: %w", err)
	}
	if !sinceTime.IsZero() && !untilTime.IsZero() &&
		!sinceTime.Before(untilTime) {
		return "", errors.New("since must be earlier than until")
	}

	group, err := loadGroup(ctx, nk, request.GroupID)
	if err != nil {
		return "", err
	}
	if group == nil {
		return "", errors.New("group not found")
	}

	member, _, _, err := groupMembership(ctx, nk, request.GroupID, userID)
	if err != nil {
		return "", err
	}
	if !member {
		return "", errors.New("current group membership is required to read history")
	}

	_, access, err := readGroupAccess(ctx, nk, request.GroupID, userID)
	if err != nil {
		return "", err
	}
	if access.JoinedAt == "" || access.RevokedAt != "" {
		return "", errors.New("active group history access is required")
	}

	joinedAt, err := time.Parse(time.RFC3339Nano, access.JoinedAt)
	if err != nil {
		return "", errors.New("group access record has an invalid join timestamp")
	}
	joinedAt = joinedAt.UTC()

	if !untilTime.IsZero() && !joinedAt.Before(untilTime) {
		data, err := json.Marshal(groupHistoryResponse{
			Messages: []groupMessageRecord{},
		})
		if err != nil {
			return "", fmt.Errorf("encode empty group history response: %w", err)
		}
		return string(data), nil
	}

	cursor := groupHistoryCursor{
		Version: 1,
		GroupID: request.GroupID,
		UserID:  userID,
		Since:   sinceValue,
		Until:   untilValue,
	}

	if request.Cursor != "" {
		cursor, err = decodeGroupHistoryCursor(request.Cursor)
		if err != nil {
			return "", err
		}
		if cursor.GroupID != request.GroupID ||
			cursor.UserID != userID ||
			cursor.Since != sinceValue ||
			cursor.Until != untilValue {
			return "", errors.New("group history cursor does not match this request")
		}
	}

	// Legacy cursors with pending page keys and no storage cursor
	// were already positioned at the end of StorageList.
	if request.Cursor != "" &&
		!cursor.StorageComplete &&
		cursor.StorageCursor == "" &&
		len(cursor.PendingKeys) > 0 {
		cursor.StorageComplete = true
	}

	effectiveSince := joinedAt
	if !sinceTime.IsZero() && sinceTime.After(effectiveSince) {
		effectiveSince = sinceTime
	}

	response := groupHistoryResponse{
		Messages: make([]groupMessageRecord, 0, request.Limit),
	}

	scanned := 0
	deferredKeys := make([]string, 0)

	processObject := func(object *api.StorageObject) error {
		record, storedAt, err := verifyGroupHistoryObject(
			ctx, nk, request.GroupID, object,
		)
		if errors.Is(err, errGroupHistoryMessagePending) {
			scanned++
			deferredKeys = append(deferredKeys, object.GetKey())
			return nil
		}
		if err != nil {
			return err
		}

		scanned++

		if storedAt.Before(effectiveSince) {
			return nil
		}
		if !untilTime.IsZero() && !storedAt.Before(untilTime) {
			return nil
		}

		response.Messages = append(response.Messages, record)
		return nil
	}

	// Resume keys from a page that did not fit in the previous response.
	pending := cursor.PendingKeys
	cursor.PendingKeys = nil

	for i, key := range pending {
		object, err := readGroupHistoryObject(ctx, nk, request.GroupID, key)
		if err != nil {
			return "", err
		}
		if err := processObject(object); err != nil {
			return "", err
		}

		if len(response.Messages) >= request.Limit {
			cursor.PendingKeys = append([]string(nil), pending[i+1:]...)
			break
		}
		if scanned >= groupHistoryMaxScan {
			cursor.PendingKeys = append([]string(nil), pending[i+1:]...)
			break
		}
	}

	pagesRead := 0
	seenStorageCursors := make(map[string]struct{})

	if cursor.StorageCursor != "" {
		seenStorageCursors[cursor.StorageCursor] = struct{}{}
	}

	for len(cursor.PendingKeys) == 0 &&
		!cursor.StorageComplete &&
		len(response.Messages) < request.Limit &&
		scanned < groupHistoryMaxScan {

		if pagesRead >= groupHistoryMaxPages {
			break
		}
		pagesRead++

		previousCursor := cursor.StorageCursor

		objects, nextCursor, err := nk.StorageList(
			ctx,
			"",
			systemStorageUserID,
			groupMessageCollection(request.GroupID),
			groupHistoryPageSize,
			previousCursor,
		)
		if err != nil {
			return "", fmt.Errorf("list group history: %w", err)
		}

		if nextCursor != "" && nextCursor == previousCursor {
			return "", errors.New("group history storage cursor did not advance")
		}
		if nextCursor != "" {
			if _, exists := seenStorageCursors[nextCursor]; exists {
				return "", errors.New("group history storage cursor repeated")
			}
			seenStorageCursors[nextCursor] = struct{}{}
		}

		cursor.StorageCursor = nextCursor
		if nextCursor == "" {
			cursor.StorageComplete = true
		}

		if len(objects) == 0 {
			if cursor.StorageComplete {
				break
			}
			continue
		}

		for i, object := range objects {
			if err := processObject(object); err != nil {
				return "", err
			}

			if len(response.Messages) >= request.Limit ||
				scanned >= groupHistoryMaxScan {
				for _, remaining := range objects[i+1:] {
					if remaining == nil ||
						!validGroupMessageKey(remaining.GetKey()) {
						return "", errors.New("invalid remaining group history key")
					}
					cursor.PendingKeys = append(
						cursor.PendingKeys,
						remaining.GetKey(),
					)
				}
				break
			}
		}

		if len(response.Messages) >= request.Limit ||
			scanned >= groupHistoryMaxScan ||
			nextCursor == "" {
			break
		}
	}

	cursor.PendingKeys = append(deferredKeys, cursor.PendingKeys...)

	hasMore := len(cursor.PendingKeys) > 0 || cursor.StorageCursor != ""
	if hasMore {
		response.Cursor, err = encodeGroupHistoryCursor(cursor)
		if err != nil {
			return "", err
		}
	}
	response.HasMore = hasMore

	data, err := json.Marshal(response)
	if err != nil {
		return "", fmt.Errorf("encode group history response: %w", err)
	}

	logger.Debug(
		"Group history read: group_id=%s user_id=%s returned=%d scanned=%d has_more=%t",
		request.GroupID, userID, len(response.Messages), scanned, response.HasMore,
	)

	return string(data), nil
}

func groupFinish(
	ctx context.Context,
	logger runtime.Logger,
	db *sql.DB,
	nk runtime.NakamaModule,
	payload string,
) (string, error) {
	userID, _, err := authenticatedSession(ctx)
	if err != nil {
		return "", err
	}

	var request groupIDRequest
	if err := decodeJSON(payload, &request); err != nil {
		return "", err
	}
	if err := validateGroupID(request.GroupID); err != nil {
		return "", err
	}

	group, err := loadGroup(ctx, nk, request.GroupID)
	if err != nil {
		return "", err
	}

	actorRole, err := requireGroupAdmin(ctx, nk, request.GroupID, userID)
	if err != nil {
		return "", err
	}
	if actorRole != "SUPERADMIN" {
		return "", errors.New("only SUPERADMIN can finish a group")
	}
	if err := ensureGroupActive(group); err != nil {
		return "", err
	}

	metadata, err := groupMetadata(group)
	if err != nil {
		return "", err
	}
	metadata["status"] = "FINISHED"
	metadata["finished_at"] = time.Now().UTC().Format(time.RFC3339Nano)

	if err := nk.GroupUpdate(
		ctx,
		request.GroupID,
		userID,
		group.GetName(),
		group.GetCreatorId(),
		group.GetLangTag(),
		group.GetDescription(),
		group.GetAvatarUrl(),
		false,
		metadata,
		int(group.GetMaxCount()),
	); err != nil {
		return "", fmt.Errorf("finish Nakama group: %w", err)
	}

	logger.Info("Group finished: group_id=%s actor=%s", request.GroupID, userID)

	data, err := json.Marshal(successResponse{Success: true})
	if err != nil {
		return "", fmt.Errorf("encode group finish response: %w", err)
	}
	return string(data), nil
}
