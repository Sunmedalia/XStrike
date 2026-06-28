package main

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
)

var operatorAuthToken string
var operatorAuthUsername string
var operatorAuthPassword string

func configureOperatorAuth(auth AuthConfig) error {
	token := strings.TrimSpace(auth.Token)
	if token == "" {
		return fmt.Errorf("%s is required for operator HTTP/WS access", authTokenEnv)
	}
	if hasNewline(token) {
		return fmt.Errorf("%s must not contain newlines", authTokenEnv)
	}
	username := strings.TrimSpace(auth.Username)
	password := auth.Password
	if hasNewline(username) || hasNewline(password) {
		return fmt.Errorf("operator auth username/password must not contain newlines")
	}
	if (username == "") != (password == "") {
		return fmt.Errorf("operator auth username and password must be configured together")
	}
	operatorAuthToken = token
	operatorAuthUsername = username
	operatorAuthPassword = password
	return nil
}

func requireOperatorAuth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if isPublicAuthRoute(r) {
			next.ServeHTTP(w, r)
			return
		}
		if isAuthorizedOperator(r) {
			next.ServeHTTP(w, r)
			return
		}
		w.Header().Set("WWW-Authenticate", `Bearer realm="ruststrike"`)
		http.Error(w, "unauthorized", http.StatusUnauthorized)
	})
}

func authLoginHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	if !passwordLoginEnabled() {
		writeAuthFailure(w, http.StatusServiceUnavailable, "password login is not configured")
		return
	}
	var body struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeAuthFailure(w, http.StatusBadRequest, "bad json: "+err.Error())
		return
	}
	if !credentialsMatch(body.Username, body.Password) {
		writeAuthFailure(w, http.StatusUnauthorized, "invalid username or password")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"success": true,
		"data": map[string]string{
			"token": operatorAuthToken,
		},
	})
}

func isPublicAuthRoute(r *http.Request) bool {
	return r.Method == http.MethodPost && (r.URL.Path == "/api/auth/login" || r.URL.Path == "/auth/login")
}

func isAuthorizedOperator(r *http.Request) bool {
	return tokenMatches(requestAuthToken(r), operatorAuthToken)
}

func requestAuthToken(r *http.Request) string {
	if token, ok := bearerToken(r.Header.Get("Authorization")); ok {
		return token
	}
	if token := strings.TrimSpace(r.Header.Get("X-RustStrike-Token")); token != "" {
		return token
	}
	if r.URL != nil && r.URL.Path == "/ws" {
		if token := strings.TrimSpace(r.URL.Query().Get("access_token")); token != "" {
			return token
		}
		if token := strings.TrimSpace(r.URL.Query().Get("token")); token != "" {
			return token
		}
	}
	return ""
}

func bearerToken(header string) (string, bool) {
	parts := strings.Fields(header)
	if len(parts) != 2 || !strings.EqualFold(parts[0], "Bearer") {
		return "", false
	}
	token := strings.TrimSpace(parts[1])
	return token, token != ""
}

func tokenMatches(got, want string) bool {
	if got == "" || want == "" {
		return false
	}
	gotHash := sha256.Sum256([]byte(got))
	wantHash := sha256.Sum256([]byte(want))
	return subtle.ConstantTimeCompare(gotHash[:], wantHash[:]) == 1
}

func passwordLoginEnabled() bool {
	return operatorAuthUsername != "" && operatorAuthPassword != ""
}

func credentialsMatch(username, password string) bool {
	if !passwordLoginEnabled() {
		return false
	}
	return tokenMatches(strings.TrimSpace(username), operatorAuthUsername) && tokenMatches(password, operatorAuthPassword)
}

func writeAuthFailure(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]any{
		"success": false,
		"error":   msg,
	})
}

func hasNewline(s string) bool {
	return strings.ContainsAny(s, "\r\n")
}
