// Эндпоинты HuntQueue (батч-воронка) и Invariants (генерация харнесса).
// По AD-5 — тонкие: гоняют бинарь `sca`, классифицируют вывод, отдают JSON.
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

// ── /api/batch ── дешёвая детерминированная воронка по пачке адресов (как hunt_batch.sh)
type batchReq struct {
	Targets []string `json:"targets"` // адреса 0x…
	Chain   int      `json:"chain"`
}
type qrow struct {
	Target string `json:"target"`
	State  string `json:"state"` // survivor | minor | clean | skip
	Detail string `json:"detail"`
	Code   string `json:"code,omitempty"`
}

var skipRe = regexp.MustCompile(`SCSA_SKIP:([A-Z0-9]+) ?(.*)`)

// запуск бинаря: возвращает stdout/stderr, exit-код и флаг «не смог запуститься/таймаут».
func runSca(ctx context.Context, dir string, args ...string) (stdout, stderr string, code int, failed bool) {
	cmd := exec.CommandContext(ctx, scaBin, args...)
	cmd.Env = os.Environ()
	if dir != "" {
		cmd.Dir = dir
	}
	var so, se strings.Builder
	cmd.Stdout, cmd.Stderr = &so, &se
	if err := cmd.Run(); err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			return so.String(), se.String(), ee.ExitCode(), false
		}
		return so.String(), se.String(), -1, true
	}
	return so.String(), se.String(), 0, false
}

func handleBatch(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req batchReq
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<16)).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "неверный JSON")
		return
	}
	chain := req.Chain
	if chain <= 0 {
		chain = 1
	}
	var targets []string
	for _, t := range req.Targets {
		if t = strings.TrimSpace(t); t != "" {
			targets = append(targets, t)
		}
		if len(targets) >= 15 { // потолок на пачку
			break
		}
	}
	if len(targets) == 0 {
		writeErr(w, http.StatusBadRequest, "пустой список адресов")
		return
	}

	out := make([]qrow, 0, len(targets))
	for _, t := range targets {
		row := qrow{Target: shortAddr(t)}
		if !addrRe.MatchString(t) {
			row.State, row.Code, row.Detail = "skip", "A0", "не адрес 0x…"
			out = append(out, row)
			continue
		}
		ctx, cancel := context.WithTimeout(r.Context(), 90*time.Second)
		so, se, code, failed := runSca(ctx, "", "--address", t, "--chain", strconv.Itoa(chain), "--audit", "--json")
		cancel()
		classifyRow(&row, so, se, code, failed)
		out = append(out, row)
	}
	writeJSON(w, http.StatusOK, out)
}

func classifyRow(row *qrow, stdout, stderr string, code int, failed bool) {
	if failed {
		if c, d := parseSkip(stderr); c != "" {
			row.State, row.Code, row.Detail = "skip", c, d
		} else {
			row.State, row.Detail = "skip", "ошибка/таймаут скана"
		}
		return
	}
	if code != 0 && code != 2 { // не 0 (чисто) и не 2 (находки) → скип
		if c, d := parseSkip(stderr); c != "" {
			row.State, row.Code, row.Detail = "skip", c, d
		} else {
			row.State, row.Detail = "skip", firstLine(strings.TrimSpace(stderr))
		}
		return
	}
	fs, err := parseFindings([]byte(stdout))
	if err != nil {
		row.State, row.Detail = "skip", "не разобран вывод"
		return
	}
	high, med, low := 0, 0, 0
	rules := map[string]struct{}{}
	for _, f := range fs {
		switch f.Severity {
		case "HIGH":
			high++
		case "MEDIUM":
			med++
		default:
			low++
		}
		rules[f.Rule] = struct{}{}
	}
	switch {
	case high > 0:
		row.State = "survivor"
		row.Detail = fmt.Sprintf("%d HIGH · %s", high, joinRules(rules))
	case med > 0 || low > 0:
		row.State = "minor"
		row.Detail = fmt.Sprintf("%d MED · %d LOW", med, low)
	default:
		row.State = "clean"
		row.Detail = "no findings — lowered OK"
	}
}

func parseSkip(stderr string) (code, detail string) {
	if mm := skipRe.FindStringSubmatch(stderr); mm != nil {
		d := strings.TrimSpace(mm[2])
		if len(d) > 60 {
			d = d[:60]
		}
		return mm[1], d
	}
	return "", ""
}

func joinRules(set map[string]struct{}) string {
	var rs []string
	for r := range set {
		rs = append(rs, r)
	}
	sort.Strings(rs)
	if len(rs) > 3 {
		rs = append(rs[:3], "…")
	}
	return strings.Join(rs, " · ")
}

// ── /api/invariant ── генерация Foundry-инвариант-харнесса для цели+свойства
type invReq struct {
	Mode     string `json:"mode"` // address | source
	Address  string `json:"address"`
	Chain    int    `json:"chain"`
	Source   string `json:"source"`
	Property string `json:"property"`
}

var invProps = map[string]bool{
	"solvency": true, "inflation": true, "inflation-erc20": true, "roundtrip": true, "supply": true,
}

func handleInvariant(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var req invReq
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, int64(maxSource)+4096)).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "неверный JSON")
		return
	}
	if !invProps[req.Property] {
		writeErr(w, http.StatusBadRequest, "неизвестное свойство инварианта")
		return
	}

	var base []string
	switch req.Mode {
	case "address":
		a := strings.TrimSpace(req.Address)
		if !addrRe.MatchString(a) {
			writeErr(w, http.StatusBadRequest, "адрес должен быть 0x + 40 hex")
			return
		}
		chain := req.Chain
		if chain <= 0 {
			chain = 1
		}
		base = []string{"--address", a, "--chain", strconv.Itoa(chain)}
	case "source":
		if len(req.Source) == 0 || len(req.Source) > maxSource {
			writeErr(w, http.StatusBadRequest, "пустой или слишком большой исходник")
			return
		}
		f, e := os.CreateTemp("", "scsa-inv-*.sol")
		if e != nil {
			writeErr(w, http.StatusInternalServerError, "temp")
			return
		}
		f.WriteString(req.Source)
		f.Close()
		defer os.Remove(f.Name())
		base = []string{"--input", f.Name()}
	default:
		writeErr(w, http.StatusBadRequest, `mode должен быть "address" или "source"`)
		return
	}

	dir, err := os.MkdirTemp("", "scsa-invh-")
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "temp")
		return
	}
	defer os.RemoveAll(dir)

	args := append(append([]string{}, base...), "--emit-invariant", "--invariant-property", req.Property)
	ctx, cancel := context.WithTimeout(r.Context(), scanTimeout)
	defer cancel()
	so, se, code, failed := runSca(ctx, dir, args...)
	if failed {
		writeErr(w, http.StatusGatewayTimeout, "скан не завершился (таймаут?)")
		return
	}
	if code != 0 && code != 2 {
		if c, d := parseSkip(se); c != "" {
			writeErr(w, http.StatusBadGateway, "пропущено ["+c+"] "+d)
			return
		}
		writeErr(w, http.StatusBadGateway, firstLine(strings.TrimSpace(se)))
		return
	}
	matches, _ := filepath.Glob(filepath.Join(dir, "*.t.sol"))
	harness, name := "", ""
	if len(matches) > 0 {
		if b, e := os.ReadFile(matches[0]); e == nil {
			harness, name = string(b), filepath.Base(matches[0])
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"harness": harness,
		"name":    name,
		"report":  strings.TrimSpace(so),
	})
}
