package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const (
	configEnv       = "RUSTSTRIKE_CONFIG"
	defaultConfig   = "ruststrike.config.json"
	authUsernameEnv = "RUSTSTRIKE_AUTH_USERNAME"
	authPasswordEnv = "RUSTSTRIKE_AUTH_PASSWORD"
	authTokenEnv    = "RUSTSTRIKE_AUTH_TOKEN"
	tcpPortEnv      = "RUSTSTRIKE_TCP_PORT"
	httpPortEnv     = "RUSTSTRIKE_HTTP_PORT"
	tcpBindEnv      = "RUSTSTRIKE_TCP_BIND_IP"
	httpBindEnv     = "RUSTSTRIKE_HTTP_BIND_IP"
	bofDirEnv       = "RUSTSTRIKE_BOFS"
	dbPathEnv       = "RUSTSTRIKE_DB"
	implantExeEnv   = "RUSTSTRIKE_IMPLANT_EXE"
)

type AppConfig struct {
	ConfigPath string       `json:"-"`
	Auth       AuthConfig   `json:"auth"`
	Server     ServerConfig `json:"server"`
	Paths      PathConfig   `json:"paths"`
}

type AuthConfig struct {
	Username string `json:"username"`
	Password string `json:"password"`
	Token    string `json:"token"`
}

type ServerConfig struct {
	ImplantBindIP       string `json:"implant_bind_ip"`
	ImplantTCPPort      string `json:"implant_tcp_port"`
	OperatorBindIP      string `json:"operator_bind_ip"`
	OperatorHTTPPort    string `json:"operator_http_port"`
	DefaultListenerName string `json:"default_listener_name"`
}

type PathConfig struct {
	BOFDir     string `json:"bof_dir"`
	DBPath     string `json:"db_path"`
	ImplantExe string `json:"implant_exe"`
}

func loadAppConfig(args []string) (AppConfig, error) {
	cfg := defaultAppConfig()
	configPath, positional, err := splitConfigArg(args)
	if err != nil {
		return cfg, err
	}
	if configPath == "" {
		configPath = strings.TrimSpace(os.Getenv(configEnv))
	}
	if configPath == "" {
		configPath = findDefaultConfig()
	}
	if configPath != "" {
		if err := readConfigFile(configPath, &cfg); err != nil {
			return cfg, err
		}
		abs, err := filepath.Abs(configPath)
		if err == nil {
			configPath = abs
		}
		cfg.ConfigPath = configPath
	}

	applyEnvConfig(&cfg)
	applyPositionalPorts(&cfg, positional)
	normalizeConfig(&cfg)
	if err := validateConfig(cfg); err != nil {
		return cfg, err
	}
	return cfg, nil
}

func defaultAppConfig() AppConfig {
	return AppConfig{
		Server: ServerConfig{
			ImplantBindIP:       "0.0.0.0",
			ImplantTCPPort:      "4444",
			OperatorBindIP:      "0.0.0.0",
			OperatorHTTPPort:    "8080",
			DefaultListenerName: "default",
		},
	}
}

func splitConfigArg(args []string) (string, []string, error) {
	var configPath string
	positional := make([]string, 0, len(args))
	for i := 0; i < len(args); i++ {
		arg := args[i]
		switch {
		case arg == "-config" || arg == "--config":
			if i+1 >= len(args) {
				return "", nil, errors.New("-config requires a path")
			}
			i++
			configPath = args[i]
		case strings.HasPrefix(arg, "-config="):
			configPath = strings.TrimPrefix(arg, "-config=")
		case strings.HasPrefix(arg, "--config="):
			configPath = strings.TrimPrefix(arg, "--config=")
		default:
			positional = append(positional, arg)
		}
	}
	return strings.TrimSpace(configPath), positional, nil
}

func findDefaultConfig() string {
	candidates := []string{defaultConfig}
	if exe, err := os.Executable(); err == nil {
		candidates = append(candidates, filepath.Join(filepath.Dir(exe), defaultConfig))
	}
	for _, p := range candidates {
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return ""
}

func readConfigFile(path string, cfg *AppConfig) error {
	raw, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("read config %s: %w", path, err)
	}
	dec := json.NewDecoder(strings.NewReader(string(raw)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(cfg); err != nil {
		return fmt.Errorf("parse config %s: %w", path, err)
	}
	return nil
}

func applyEnvConfig(cfg *AppConfig) {
	setFromEnv(&cfg.Auth.Username, authUsernameEnv)
	setFromEnv(&cfg.Auth.Password, authPasswordEnv)
	setFromEnv(&cfg.Auth.Token, authTokenEnv)
	setFromEnv(&cfg.Server.ImplantTCPPort, tcpPortEnv)
	setFromEnv(&cfg.Server.OperatorHTTPPort, httpPortEnv)
	setFromEnv(&cfg.Server.ImplantBindIP, tcpBindEnv)
	setFromEnv(&cfg.Server.OperatorBindIP, httpBindEnv)
	setFromEnv(&cfg.Paths.BOFDir, bofDirEnv)
	setFromEnv(&cfg.Paths.DBPath, dbPathEnv)
	setFromEnv(&cfg.Paths.ImplantExe, implantExeEnv)
}

func setFromEnv(dst *string, key string) {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		*dst = v
	}
}

func applyPositionalPorts(cfg *AppConfig, positional []string) {
	if len(positional) > 0 {
		cfg.Server.ImplantTCPPort = positional[0]
	}
	if len(positional) > 1 {
		cfg.Server.OperatorHTTPPort = positional[1]
	}
}

func normalizeConfig(cfg *AppConfig) {
	cfg.Auth.Username = strings.TrimSpace(cfg.Auth.Username)
	cfg.Auth.Token = strings.TrimSpace(cfg.Auth.Token)
	cfg.Server.ImplantBindIP = strings.TrimSpace(cfg.Server.ImplantBindIP)
	cfg.Server.ImplantTCPPort = strings.TrimSpace(cfg.Server.ImplantTCPPort)
	cfg.Server.OperatorBindIP = strings.TrimSpace(cfg.Server.OperatorBindIP)
	cfg.Server.OperatorHTTPPort = strings.TrimSpace(cfg.Server.OperatorHTTPPort)
	cfg.Server.DefaultListenerName = strings.TrimSpace(cfg.Server.DefaultListenerName)
	cfg.Paths.BOFDir = strings.TrimSpace(cfg.Paths.BOFDir)
	cfg.Paths.DBPath = strings.TrimSpace(cfg.Paths.DBPath)
	cfg.Paths.ImplantExe = strings.TrimSpace(cfg.Paths.ImplantExe)
	if cfg.Server.ImplantBindIP == "" {
		cfg.Server.ImplantBindIP = "0.0.0.0"
	}
	if cfg.Server.OperatorBindIP == "" {
		cfg.Server.OperatorBindIP = "0.0.0.0"
	}
	if cfg.Server.DefaultListenerName == "" {
		cfg.Server.DefaultListenerName = "default"
	}
}

func validateConfig(cfg AppConfig) error {
	if cfg.Server.ImplantTCPPort == "" {
		return errors.New("server.implant_tcp_port is required")
	}
	if cfg.Server.OperatorHTTPPort == "" {
		return errors.New("server.operator_http_port is required")
	}
	return nil
}
