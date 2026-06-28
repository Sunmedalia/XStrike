package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestRequireOperatorAuthRejectsMissingAndWrongToken(t *testing.T) {
	operatorAuthToken = "test-secret"
	handler := requireOperatorAuth(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))

	for _, tc := range []struct {
		name   string
		header string
	}{
		{name: "missing"},
		{name: "wrong", header: "Bearer wrong-secret"},
		{name: "bad scheme", header: "Basic test-secret"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, "/api/implants", nil)
			if tc.header != "" {
				req.Header.Set("Authorization", tc.header)
			}
			rr := httptest.NewRecorder()
			handler.ServeHTTP(rr, req)
			if rr.Code != http.StatusUnauthorized {
				t.Fatalf("status = %d, want %d", rr.Code, http.StatusUnauthorized)
			}
		})
	}
}

func TestRequireOperatorAuthAllowsBearerToken(t *testing.T) {
	operatorAuthToken = "test-secret"
	handler := requireOperatorAuth(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))

	req := httptest.NewRequest(http.MethodGet, "/api/implants", nil)
	req.Header.Set("Authorization", "Bearer test-secret")
	rr := httptest.NewRecorder()
	handler.ServeHTTP(rr, req)

	if rr.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want %d", rr.Code, http.StatusNoContent)
	}
}

func TestWebSocketQueryTokenIsScopedToWS(t *testing.T) {
	operatorAuthToken = "test-secret"
	handler := requireOperatorAuth(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))

	wsReq := httptest.NewRequest(http.MethodGet, "/ws?token=test-secret", nil)
	wsRR := httptest.NewRecorder()
	handler.ServeHTTP(wsRR, wsReq)
	if wsRR.Code != http.StatusNoContent {
		t.Fatalf("ws status = %d, want %d", wsRR.Code, http.StatusNoContent)
	}

	apiReq := httptest.NewRequest(http.MethodGet, "/api/implants?token=test-secret", nil)
	apiRR := httptest.NewRecorder()
	handler.ServeHTTP(apiRR, apiReq)
	if apiRR.Code != http.StatusUnauthorized {
		t.Fatalf("api status = %d, want %d", apiRR.Code, http.StatusUnauthorized)
	}
}

func TestAuthLoginReturnsConfiguredToken(t *testing.T) {
	operatorAuthToken = "test-secret"
	operatorAuthUsername = "admin"
	operatorAuthPassword = "passw0rd"
	req := httptest.NewRequest(http.MethodPost, "/api/auth/login", strings.NewReader(`{"username":"admin","password":"passw0rd"}`))
	rr := httptest.NewRecorder()

	authLoginHandler(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", rr.Code, http.StatusOK)
	}
	var body struct {
		Success bool `json:"success"`
		Data    struct {
			Token string `json:"token"`
		} `json:"data"`
	}
	if err := json.NewDecoder(rr.Body).Decode(&body); err != nil {
		t.Fatal(err)
	}
	if !body.Success || body.Data.Token != "test-secret" {
		t.Fatalf("body = %+v, want success token", body)
	}
}

func TestAuthLoginRejectsBadPassword(t *testing.T) {
	operatorAuthToken = "test-secret"
	operatorAuthUsername = "admin"
	operatorAuthPassword = "passw0rd"
	req := httptest.NewRequest(http.MethodPost, "/api/auth/login", strings.NewReader(`{"username":"admin","password":"wrong"}`))
	rr := httptest.NewRecorder()

	authLoginHandler(rr, req)

	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want %d", rr.Code, http.StatusUnauthorized)
	}
}

func TestLoginRouteBypassesBearerMiddleware(t *testing.T) {
	operatorAuthToken = "test-secret"
	operatorAuthUsername = "admin"
	operatorAuthPassword = "passw0rd"
	mux := http.NewServeMux()
	mux.HandleFunc("/api/auth/login", authLoginHandler)
	handler := requireOperatorAuth(mux)

	req := httptest.NewRequest(http.MethodPost, "/api/auth/login", strings.NewReader(`{"username":"admin","password":"passw0rd"}`))
	rr := httptest.NewRecorder()
	handler.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", rr.Code, http.StatusOK)
	}
}
