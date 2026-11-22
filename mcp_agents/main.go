package main

import (
	"context"
	"database/sql"
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

	maxMessageSize    = 1024 * 1024
	maxIdentifierSize = 256
	maxCapabilities   = 64
)

type agentIdentity struct {
	AgentID    string `json:"agent_id"`
	InstanceID string `json:"instance_id"`
}

type agentMetadata struct {
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

	// Relay the original envelope unchanged.
	if err := nk.StreamSend(
		agentStreamMode,
		"",
		"",
		agentStreamLabel,
		payload,
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
