// SCSA web backend — ТОНКИЙ слой (AD-5): не содержит логики анализа.
// Только: (1) дёргает бинарь `solidity-llvm` (продукт), (2) парсит его JSON,
// (3) маппит в модель фронта, (4) раздаёт собранный React (web/dist).
// Логику анализа НИКОГДА сюда не переносить — она живёт в C++-бинаре.
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// ── конфигурация из окружения ─────────────────────────────────────────────
var (
	addr        = envOr("PORT", "8080")
	scaBin      = envOr("SCA_BIN", "../../solidity-llvm/build/src/solidity-llvm")
	webDist     = envOr("WEB_DIST", "../dist")
	scanTimeout = envDuration("SCAN_TIMEOUT", 240*time.Second)
	maxSource   = 2 << 20 // 2 МБ — потолок на присланный исходник
)

// адрес контракта: 0x + 40 hex. защита от инъекции в аргументы бинаря.
var addrRe = regexp.MustCompile(`^0x[0-9a-fA-F]{40}$`)

// ── модели ────────────────────────────────────────────────────────────────

// то, что эмитит бинарь (`--json`, см. report/JsonReporter.cpp)
type toolFinding struct {
	Rule          string `json:"rule"`
	Severity      string `json:"severity"` // HIGH | MEDIUM | LOW
	Function      string `json:"function"`
	Line          int    `json:"line"`
	StorageSlot   int    `json:"storageSlot"`
	SymConfirmed  bool   `json:"symConfirmed"`
	PriorityScore int    `json:"priorityScore"`
	Message       string `json:"message"`
}

// то, что ждёт фронт (см. web/src/data.ts)
type finding struct {
	ID      string `json:"id"`
	Sev     string `json:"sev"`    // HIGH | MED | LOW
	Rule    string `json:"rule"`
	Fn      string `json:"fn"`     // "setOwner()"
	Line    int    `json:"line"`
	Status  string `json:"status"` // klee | candidate | info
	Score   int    `json:"score"`
	Message string `json:"message"`
}

type target struct {
	Name   string `json:"name"`
	Source string `json:"source"`
	Chain  int    `json:"chain"`
}

type scanRequest struct {
	Mode    string `json:"mode"` // "address" | "source"
	Address string `json:"address"`
	Chain   int    `json:"chain"`
	Source  string `json:"source"`
	Name    string `json:"name"`
}

type scanResponse struct {
	Target   target    `json:"target"`
	Findings []finding `json:"findings"`
}

// ── main ────────────────────────────────────────────────────────────────────

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/health", handleHealth)
	mux.HandleFunc("/api/scan", handleScan)
	mux.HandleFunc("/api/batch", handleBatch)         // HuntQueue: воронка по пачке адресов
	mux.HandleFunc("/api/invariant", handleInvariant) // Invariants: генерация харнесса
	mux.HandleFunc("/", handleStatic)                 // SPA + статика

	bin, _ := filepath.Abs(scaBin)
	log.Printf("SCSA backend :%s  → бинарь %s  → статика %s", addr, bin, webDist)
	if _, err := os.Stat(scaBin); err != nil {
		log.Printf("ВНИМАНИЕ: бинарь не найден по SCA_BIN=%s — /api/scan вернёт 500", scaBin)
	}
	if err := http.ListenAndServe(":"+addr, withCORS(mux)); err != nil {
		log.Fatal(err)
	}
}

// ── эндпоинты ────────────────────────────────────────────────────────────────

func handleHealth(w http.ResponseWriter, _ *http.Request) {
	_, err := os.Stat(scaBin)
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":     err == nil,
		"binary": scaBin,
	})
}

func handleScan(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req scanRequest
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, int64(maxSource)+4096)).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "неверный JSON запроса")
		return
	}

	// собираем аргументы бинаря — БЕЗ шелла, каждый аргумент отдельно
	args, tmp, err := buildArgs(&req)
	if tmp != "" {
		defer os.Remove(tmp)
	}
	if err != nil {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), scanTimeout)
	defer cancel()

	cmd := exec.CommandContext(ctx, scaBin, args...)
	cmd.Env = os.Environ() // пробрасываем ETHERSCAN_API_KEY и пр.
	var stdout, stderr bytes.Buffer
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	runErr := cmd.Run()

	if ctx.Err() == context.DeadlineExceeded {
		writeErr(w, http.StatusGatewayTimeout, "скан превысил таймаут")
		return
	}
	// exit-код 2 = «находки есть» (см. Driver), это НЕ ошибка. прочие ненулевые — ошибка.
	if runErr != nil {
		if ee, ok := runErr.(*exec.ExitError); !ok || ee.ExitCode() != 2 {
			log.Printf("scan failed: %v · stderr: %s", runErr, trim(stderr.String()))
			writeErr(w, http.StatusBadGateway, "бинарь вернул ошибку: "+trim(firstLine(stderr.String())))
			return
		}
	}

	tfs, perr := parseFindings(stdout.Bytes())
	if perr != nil {
		log.Printf("parse failed: %v · out: %s", perr, trim(stdout.String()))
		writeErr(w, http.StatusBadGateway, "не удалось разобрать вывод бинаря")
		return
	}

	writeJSON(w, http.StatusOK, scanResponse{
		Target:   makeTarget(&req),
		Findings: mapFindings(tfs),
	})
}

// ── сборка аргументов ────────────────────────────────────────────────────────

func buildArgs(req *scanRequest) (args []string, tmpFile string, err error) {
	switch req.Mode {
	case "address":
		a := strings.TrimSpace(req.Address)
		if !addrRe.MatchString(a) {
			return nil, "", errStr("адрес должен быть вида 0x + 40 hex")
		}
		chain := req.Chain
		if chain <= 0 {
			chain = 1
		}
		return []string{"--address", a, "--chain", strconv.Itoa(chain), "--audit", "--json"}, "", nil

	case "source":
		if len(req.Source) == 0 {
			return nil, "", errStr("пустой исходник")
		}
		if len(req.Source) > maxSource {
			return nil, "", errStr("исходник больше лимита (2 МБ)")
		}
		f, e := os.CreateTemp("", "scsa-scan-*.sol")
		if e != nil {
			return nil, "", errStr("не удалось создать временный файл")
		}
		if _, e = f.WriteString(req.Source); e != nil {
			f.Close()
			os.Remove(f.Name())
			return nil, "", errStr("не удалось записать исходник")
		}
		f.Close()
		return []string{"--input", f.Name(), "--audit", "--json"}, f.Name(), nil

	default:
		return nil, "", errStr(`mode должен быть "address" или "source"`)
	}
}

func makeTarget(req *scanRequest) target {
	if req.Mode == "address" {
		a := strings.TrimSpace(req.Address)
		chain := req.Chain
		if chain <= 0 {
			chain = 1
		}
		return target{Name: shortAddr(a), Source: "etherscan · chain " + strconv.Itoa(chain), Chain: chain}
	}
	name := req.Name
	if name == "" {
		name = "contract.sol"
	}
	return target{Name: name, Source: "uploaded · sca --audit", Chain: 0}
}

// ── маппинг вывода бинаря → модель фронта ─────────────────────────────────────

func parseFindings(out []byte) ([]toolFinding, error) {
	// вывод бинаря — JSON-массив; отрезаем возможную преамбулу до первой '['
	i := bytes.IndexByte(out, '[')
	if i < 0 {
		return nil, errStr("в выводе нет JSON-массива")
	}
	var tfs []toolFinding
	// Decoder читает ОДНО значение и игнорирует хвост — устойчиво к лишним строкам
	if err := json.NewDecoder(bytes.NewReader(out[i:])).Decode(&tfs); err != nil {
		return nil, err
	}
	return tfs, nil
}

func mapFindings(tfs []toolFinding) []finding {
	out := make([]finding, 0, len(tfs))
	for idx, t := range tfs {
		out = append(out, finding{
			ID:      "f" + strconv.Itoa(idx+1),
			Sev:     mapSev(t.Severity),
			Rule:    t.Rule,
			Fn:      t.Function + "()",
			Line:    t.Line,
			Status:  mapStatus(t),
			Score:   t.PriorityScore,
			Message: t.Message,
		})
	}
	return out
}

func mapSev(s string) string {
	if s == "MEDIUM" {
		return "MED"
	}
	return s // HIGH | LOW
}

func mapStatus(t toolFinding) string {
	if t.SymConfirmed {
		return "klee"
	}
	if t.Severity == "LOW" {
		return "info"
	}
	return "candidate"
}

// ── статика (SPA fallback) ────────────────────────────────────────────────────

func handleStatic(w http.ResponseWriter, r *http.Request) {
	clean := filepath.Clean(r.URL.Path)
	p := filepath.Join(webDist, clean)
	// путь-траверсал наружу dist запрещаем
	if !strings.HasPrefix(p, filepath.Clean(webDist)) {
		http.NotFound(w, r)
		return
	}
	if fi, err := os.Stat(p); err == nil && !fi.IsDir() {
		http.ServeFile(w, r, p)
		return
	}
	// маршрут SPA — отдаём index.html (роутинг на клиенте)
	http.ServeFile(w, r, filepath.Join(webDist, "index.html"))
}

// ── утилиты ───────────────────────────────────────────────────────────────────

func withCORS(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// для dev-режима (vite :5173 → бэк :8080). в проде фронт раздаёт этот же бэк
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		h.ServeHTTP(w, r)
	})
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}

type strErr string

func (e strErr) Error() string { return string(e) }
func errStr(s string) error    { return strErr(s) }

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func envDuration(k string, def time.Duration) time.Duration {
	if v := os.Getenv(k); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			return d
		}
	}
	return def
}

func shortAddr(a string) string {
	if len(a) <= 12 {
		return a
	}
	return a[:6] + "…" + a[len(a)-4:]
}

func trim(s string) string {
	s = strings.TrimSpace(s)
	if len(s) > 400 {
		return s[:400] + "…"
	}
	return s
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return s[:i]
	}
	return s
}
