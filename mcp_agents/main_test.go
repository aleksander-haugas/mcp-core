package main

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
)

const testTimestamp = "2026-11-22T19:00:00Z"

func testEnvelope(kind, extra, payload string) string {
	base := fmt.Sprintf(
		`{"protocol":"agent","version":1,"type":%q,`+
			`"message_id":"msg-001",`+
			`"source":{"agent_id":"monitor","instance_id":"monitor-001"},`,
		kind,
	)

	switch kind {
	case "request":
		base += `"target":{"agent_id":"guardian","instance_id":"guardian-001"},` +
			`"method":"system.health",`
	case "response", "error":
		base += `"target":{"agent_id":"guardian","instance_id":"guardian-001"},` +
			`"correlation_id":"msg-001",`
	case "event":
		base += `"method":"system.health",`
	}

	return base + fmt.Sprintf(
		`"payload":%s,"timestamp":%q%s}`,
		payload, testTimestamp, extra,
	)
}

func TestValidIdentifier(t *testing.T) {
	cases := []struct {
		name  string
		value string
		valid bool
	}{
		{"empty", "", false},
		{"normal", "agent-01", true},
		{"256 bytes", strings.Repeat("a", 256), true},
		{"257 bytes", strings.Repeat("a", 257), false},
		{"control", "agent\n01", false},
		{"DEL", "agent\x7f", false},
		{"UTF8", "agént", true},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := validIdentifier(tc.value); got != tc.valid {
				t.Fatalf("validIdentifier(%q) = %v, want %v",
					tc.value, got, tc.valid)
			}
		})
	}
}

func TestValidMethod(t *testing.T) {
	valid := []string{
		"system.health",
		"_system.health_check",
		"agent.v2-health",
		"a.b",
	}
	invalid := []string{
		"", "health", ".health", "system.", "system..health",
		"1system.health", "system.2health", "system.health!",
		"system.é", strings.Repeat("a", 254) + ".xy",
	}

	for _, value := range valid {
		if !validMethod(value) {
			t.Errorf("expected valid method: %q", value)
		}
	}
	for _, value := range invalid {
		if validMethod(value) {
			t.Errorf("expected invalid method: %q", value)
		}
	}
}

func TestProtocolTimestamp(t *testing.T) {
	valid := []string{
		"2026-11-22T19:00:00Z",
		"2024-02-29T23:59:59Z",
		"2026-11-22T19:00:00.1Z",
		"2026-11-22T19:00:00.12345678901234567890Z",
	}
	invalid := []string{
		"",
		"0000-01-01T00:00:00Z",
		"2023-02-29T00:00:00Z",
		"2026-13-01T00:00:00Z",
		"2026-11-31T00:00:00Z",
		"2026-11-22T24:00:00Z",
		"2026-11-22T19:60:00Z",
		"2026-11-22T19:00:60Z",
		"2026-11-22T19:00:00+00:00",
		"2026-11-22t19:00:00Z",
		"2026-11-22T19:00:00.Z",
		"2026-11-22T19:00:00.1",
	}

	for _, value := range valid {
		if !validProtocolTimestamp(value) {
			t.Errorf("expected valid timestamp: %q", value)
		}
	}
	for _, value := range invalid {
		if validProtocolTimestamp(value) {
			t.Errorf("expected invalid timestamp: %q", value)
		}
	}
}

func TestValidateEnvelopeMessageTypes(t *testing.T) {
	cases := []struct {
		kind    string
		payload string
	}{
		{"request", `{"detail":"full"}`},
		{"response", `{"status":"ok"}`},
		{"event", `{"status":"online"}`},
		{"error", `{"code":"AGENT_ERROR","message":"failed"}`},
	}

	for _, tc := range cases {
		t.Run(tc.kind, func(t *testing.T) {
			input := testEnvelope(tc.kind, "", tc.payload)
			if _, err := validateEnvelope(input); err != nil {
				t.Fatalf("valid %s rejected: %v", tc.kind, err)
			}
		})
	}
}

func TestValidateEnvelopeRejectsInvalidCombinations(t *testing.T) {
	cases := []struct {
		name  string
		input string
	}{
		{
			"request with correlation",
			`{"protocol":"agent","version":1,"type":"request",` +
				`"message_id":"m","source":{"agent_id":"a","instance_id":"i"},` +
				`"target":{"agent_id":"b","instance_id":"j"},` +
				`"method":"system.health","correlation_id":"c",` +
				`"payload":{},"timestamp":"` + testTimestamp + `"}`,
		},
		{
			"response with method",
			`{"protocol":"agent","version":1,"type":"response",` +
				`"message_id":"m","source":{"agent_id":"a","instance_id":"i"},` +
				`"target":{"agent_id":"b","instance_id":"j"},` +
				`"method":"system.health","correlation_id":"c",` +
				`"payload":{},"timestamp":"` + testTimestamp + `"}`,
		},
		{
			"event with correlation",
			`{"protocol":"agent","version":1,"type":"event",` +
				`"message_id":"m","source":{"agent_id":"a","instance_id":"i"},` +
				`"method":"system.health","correlation_id":"c",` +
				`"payload":{},"timestamp":"` + testTimestamp + `"}`,
		},
		{
			"error without target",
			`{"protocol":"agent","version":1,"type":"error",` +
				`"message_id":"m","source":{"agent_id":"a","instance_id":"i"},` +
				`"correlation_id":"c","payload":{"code":"ERR","message":"x"},` +
				`"timestamp":"` + testTimestamp + `"}`,
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := validateEnvelope(tc.input); err == nil {
				t.Fatal("expected validation failure")
			}
		})
	}
}

func TestErrorPayloadValidation(t *testing.T) {
	valid := []string{
		`{"code":"ERROR","message":"failed"}`,
		`{"code":"E1","message":"failed","details":{}}`,
		`{"code":"ERROR","message":"failed","extra":true}`,
	}
	invalid := []string{
		`null`,
		`[]`,
		`{}`,
		`{"code":1,"message":"failed"}`,
		`{"code":"lowercase","message":"failed"}`,
		`{"code":"BAD-CODE","message":"failed"}`,
		`{"code":"","message":"failed"}`,
		`{"code":"ERROR","message":""}`,
		`{"code":"ERROR","message":"failed","details":null}`,
		`{"code":"ERROR","message":"failed","details":[]}`,
		`{"code":"ERROR","message":"failed","details":"x"}`,
	}

	for _, value := range valid {
		if err := validateErrorPayload([]byte(value)); err != nil {
			t.Errorf("valid error payload rejected %s: %v", value, err)
		}
	}
	for _, value := range invalid {
		if err := validateErrorPayload([]byte(value)); err == nil {
			t.Errorf("invalid error payload accepted: %s", value)
		}
	}
}

func TestProtocolJSONDuplicateKeys(t *testing.T) {
	invalid := []string{
		`{"a":1,"a":2}`,
		`{"protocol":"agent","pro\u0074ocol":"agent"}`,
		`{"outer":{"x":1,"x":2}}`,
		`{"items":[{"x":1,"x":2}]}`,
	}

	for _, value := range invalid {
		if err := validateProtocolJSON(value); err == nil {
			t.Errorf("duplicate key accepted: %s", value)
		}
	}
}

func TestProtocolJSONUnknownFieldsAndTrailingData(t *testing.T) {
	input := testEnvelope("request", `,"future_field":{"x":1}`, `{}`)
	if _, err := validateEnvelope(input); err != nil {
		t.Fatalf("unknown envelope field should be ignored: %v", err)
	}

	invalid := []string{
		testEnvelope("request", "", `{}`) + ` {}`,
		testEnvelope("request", "", `{}`) + ` trailing`,
		`{"protocol":`,
		`[]`,
		``,
	}

	for _, value := range invalid {
		if _, err := validateEnvelope(value); err == nil {
			t.Errorf("invalid document accepted: %q", value)
		}
	}
}

func TestProtocolJSONDepthLimit(t *testing.T) {
	accepted := strings.Repeat("[", 127) + "0" + strings.Repeat("]", 127)
	rejected := strings.Repeat("[", 128) + "0" + strings.Repeat("]", 128)

	// The public protocol parser requires an object at the root.
	if err := validateProtocolJSON(`{"payload":` + accepted + `}`); err != nil {
		t.Fatalf("depth 128 should be accepted: %v", err)
	}
	if err := validateProtocolJSON(`{"payload":` + rejected + `}`); err == nil {
		t.Fatal("depth 129 should be rejected")
	}
}

func TestEnvelopeMessageSizeLimit(t *testing.T) {
	oversized := strings.Repeat(" ", maxMessageSize+1)
	if _, err := validateEnvelope(oversized); err == nil {
		t.Fatal("oversized envelope should be rejected")
	}
}

func TestEnvelopeRejectsNullOptionalFields(t *testing.T) {
	cases := []struct {
		name  string
		field string
	}{
		{"null target", `"target":null,`},
		{"null correlation_id", `"correlation_id":null,`},
		{"null method", `"method":null,`},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			input := `{"protocol":"agent","version":1,"type":"event",` +
				`"message_id":"msg-001",` +
				`"source":{"agent_id":"monitor","instance_id":"monitor-001"},` +
				tc.field +
				`"payload":{},"timestamp":"2026-11-22T19:00:00Z"}`

			if _, err := validateEnvelope(input); err == nil {
				t.Fatal("present optional field with null must be rejected")
			}
		})
	}
}

func TestEnvelopeRejectsWrongOptionalFieldTypes(t *testing.T) {
	cases := []struct {
		name  string
		field string
	}{
		{"target string", `"target":"monitor",`},
		{"correlation number", `"correlation_id":123,`},
		{"method boolean", `"method":true,`},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			input := `{"protocol":"agent","version":1,"type":"event",` +
				`"message_id":"msg-001",` +
				`"source":{"agent_id":"monitor","instance_id":"monitor-001"},` +
				tc.field +
				`"payload":{},"timestamp":"2026-11-22T19:00:00Z"}`

			if _, err := validateEnvelope(input); err == nil {
				t.Fatal("wrong optional field type must be rejected")
			}
		})
	}
}

func TestValidIdentifierMatchesByteBasedCppRules(t *testing.T) {
	cases := []struct {
		name  string
		value string
		valid bool
	}{
		{"ASCII", "agent-01", true},
		{"UTF8", "agént", true},
		{"invalid UTF8 byte", string([]byte{0xff}), true},
		{"embedded control", string([]byte{'a', 0x01, 'b'}), false},
		{"DEL byte", string([]byte{'a', 0x7f}), false},
		{"empty", "", false},
		{"too long", strings.Repeat("a", 257), false},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := validIdentifier(tc.value); got != tc.valid {
				t.Fatalf("validIdentifier() = %v, want %v", got, tc.valid)
			}
		})
	}
}

func TestEncodeAgentRelay(t *testing.T) {
	envelope := `{"protocol":"agent","version":1,"message_id":"test-1"}`

	encoded, err := encodeAgentRelay("authenticated-user", "authenticated-session", envelope)
	if err != nil {
		t.Fatalf("encodeAgentRelay() error: %v", err)
	}

	var relay agentRelay
	if err := json.Unmarshal([]byte(encoded), &relay); err != nil {
		t.Fatalf("decode relay: %v", err)
	}

	if relay.Protocol != "mcp_agents_relay" || relay.Version != 1 {
		t.Fatalf("unexpected relay protocol/version: %q/%d", relay.Protocol, relay.Version)
	}
	if relay.UserID != "authenticated-user" || relay.SessionID != "authenticated-session" {
		t.Fatal("authenticated identity was not preserved")
	}

	var got, want interface{}
	if err := json.Unmarshal(relay.Envelope, &got); err != nil {
		t.Fatalf("decode inner envelope: %v", err)
	}
	if err := json.Unmarshal([]byte(envelope), &want); err != nil {
		t.Fatalf("decode expected envelope: %v", err)
	}

	gotJSON, _ := json.Marshal(got)
	wantJSON, _ := json.Marshal(want)
	if string(gotJSON) != string(wantJSON) {
		t.Fatalf("inner envelope changed: got %s, want %s", gotJSON, wantJSON)
	}
}

func TestEncodeAgentRelayRejectsInvalidInput(t *testing.T) {
	valid := `{"protocol":"agent","version":1}`

	cases := []struct {
		name      string
		userID    string
		sessionID string
		payload   string
	}{
		{"empty user", "", "session", valid},
		{"empty session", "user", "", valid},
		{"oversized user", strings.Repeat("u", maxIdentifierSize+1), "session", valid},
		{"oversized session", "user", strings.Repeat("s", maxIdentifierSize+1), valid},
		{"empty envelope", "user", "session", ""},
		{"invalid JSON", "user", "session", `{"broken":`},
		{"oversized envelope", "user", "session",
			`{"data":"` + strings.Repeat("x", maxMessageSize) + `"}`},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := encodeAgentRelay(tc.userID, tc.sessionID, tc.payload); err == nil {
				t.Fatal("invalid relay input was accepted")
			}
		})
	}
}
