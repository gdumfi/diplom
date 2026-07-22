// SCSA TUI — терминальный интерфейс сканера. Тонкая «морда» (AD-5): дёргает бинарь
// `sca --audit --json` (+ `--llm-context`/`--poc`/KLEE) и рисует находки. Запуск: `sca tui`.
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/atotto/clipboard"
	"github.com/charmbracelet/bubbles/spinner"
	"github.com/charmbracelet/bubbles/textinput"
	"github.com/charmbracelet/bubbles/viewport"
	tea "github.com/charmbracelet/bubbletea"
)

// ── модель находки (контракт `--json`, см. report/JsonReporter.cpp) ──
type finding struct {
	Rule          string `json:"rule"`
	Severity      string `json:"severity"`
	Function      string `json:"function"`
	Line          int    `json:"line"`
	StorageSlot   int    `json:"storageSlot"`
	SymConfirmed  bool   `json:"symConfirmed"`
	PriorityScore int    `json:"priorityScore"`
	Message       string `json:"message"`
}

type state int

const (
	stateInput state = iota
	stateScanning
	stateList
	stateDetail
)

type loadState int

const (
	notLoaded loadState = iota
	loading
	loaded
	failed
)

// табы детали
const (
	tabOverview = 0
	tabFacts    = 1
	tabPoc      = 2
	tabConfirm  = 3
)

var tabNames = []string{"Overview", "IR-facts", "PoC", "Confirm"}

// ── сообщения ──
type scanDoneMsg struct {
	findings []finding
	target   string
	err      error
}
type factsMsg struct {
	text string
	err  error
}
type pocMsg struct {
	text string
	err  error
}
type kleeMsg struct {
	text string
	err  error
}
type copiedMsg struct{}

type model struct {
	state    state
	target   textinput.Model
	chain    textinput.Model
	focus    int
	spin     spinner.Model
	findings []finding
	cursor   int
	vp       viewport.Model

	scanned   string // подпись цели
	reqTarget string // точная цель (для facts/poc/klee)
	reqChain  string
	isFile    bool

	detailTab  int
	facts      string
	factsState loadState
	poc        string
	pocState   loadState
	klee       string
	kleeState  loadState
	toast      string // всплывающая подсказка («скопировано»)

	err                error
	w, h               int
	scaBin, kleeScript string
}

func initialModel() model {
	t := textinput.New()
	t.Placeholder = "0x… адрес  ИЛИ  путь к .sol"
	t.Prompt = "› "
	t.Focus()
	t.CharLimit = 200
	t.Width = 44

	c := textinput.New()
	c.Placeholder = "1"
	c.SetValue("1")
	c.Prompt = "› "
	c.CharLimit = 7
	c.Width = 8

	sp := spinner.New()
	sp.Spinner = spinner.Dot
	sp.Style = stAccent

	bin := os.Getenv("SCA_BIN")
	if bin == "" {
		bin = "solidity-llvm"
	}
	klee := os.Getenv("SCSA_KLEE")
	if klee == "" {
		// …/solidity-llvm/build/src/solidity-llvm → …/solidity-llvm/benchmarks/run_klee.sh
		root := filepath.Dir(filepath.Dir(filepath.Dir(bin)))
		klee = filepath.Join(root, "benchmarks", "run_klee.sh")
	}
	return model{state: stateInput, target: t, chain: c, spin: sp, scaBin: bin, kleeScript: klee}
}

func (m model) Init() tea.Cmd { return textinput.Blink }

// ── аргументы цели ──
func (m model) baseArgs() []string {
	if m.isFile {
		return []string{"--input", m.reqTarget}
	}
	chain := m.reqChain
	if chain == "" {
		chain = "1"
	}
	return []string{"--address", m.reqTarget, "--chain", chain}
}

// run — запуск бинаря; exit-код 2 = «находки есть», не ошибка.
func run(bin string, args []string, extraEnv ...string) (string, error) {
	cmd := exec.Command(bin, args...)
	cmd.Env = append(os.Environ(), extraEnv...)
	out, err := cmd.Output()
	if err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			if ee.ExitCode() == 2 {
				return string(out), nil
			}
			if s := strings.TrimSpace(string(ee.Stderr)); s != "" {
				return string(out), fmt.Errorf("%s", firstLine(s))
			}
		}
		return string(out), err
	}
	return string(out), nil
}

func (m model) scanCmd() tea.Cmd {
	bin, args := m.scaBin, append(m.baseArgs(), "--audit", "--json")
	target := m.reqTarget
	return func() tea.Msg {
		out, err := run(bin, args)
		if err != nil {
			return scanDoneMsg{err: err}
		}
		fs, perr := parseFindings([]byte(out))
		return scanDoneMsg{findings: fs, target: target, err: perr}
	}
}

func (m model) factsCmd() tea.Cmd {
	bin, args := m.scaBin, append(m.baseArgs(), "--llm-context")
	return func() tea.Msg { out, err := run(bin, args); return factsMsg{out, err} }
}

// --poc пишет PoC в ФАЙЛ *PoC.t.sol в CWD (а не в stdout). Запускаем во временной
// папке и читаем сгенерённый файл. reqTarget для файла — абсолютный (см. updateInput).
func (m model) pocCmd() tea.Cmd {
	bin, args := m.scaBin, append(m.baseArgs(), "--poc")
	return func() tea.Msg {
		dir, err := os.MkdirTemp("", "scsa-poc-")
		if err != nil {
			return pocMsg{err: err}
		}
		defer os.RemoveAll(dir)
		cmd := exec.Command(bin, args...)
		cmd.Dir = dir
		cmd.Env = os.Environ()
		if _, e := cmd.Output(); e != nil {
			if ee, ok := e.(*exec.ExitError); !ok || ee.ExitCode() != 2 {
				s := e.Error()
				if ok && len(ee.Stderr) > 0 {
					s = firstLine(strings.TrimSpace(string(ee.Stderr)))
				}
				return pocMsg{err: fmt.Errorf("%s", s)}
			}
		}
		matches, _ := filepath.Glob(filepath.Join(dir, "*.t.sol"))
		if len(matches) == 0 {
			return pocMsg{text: "PoC не сгенерирован для этого класса/контракта (пока каркас).\nАвто-PoC: reentrancy и access-control на deposit+mapping."}
		}
		b, _ := os.ReadFile(matches[0])
		return pocMsg{text: string(b)}
	}
}

func (m model) kleeCmd(fn string) tea.Cmd {
	script, file, bin := m.kleeScript, m.reqTarget, m.scaBin
	return func() tea.Msg {
		out, err := run("bash", []string{script, file, fn}, "SCSA_BIN="+bin)
		return kleeMsg{out, err}
	}
}

func parseFindings(out []byte) ([]finding, error) {
	i := bytes.IndexByte(out, '[')
	if i < 0 {
		return nil, fmt.Errorf("в выводе нет JSON")
	}
	var fs []finding
	if err := json.NewDecoder(bytes.NewReader(out[i:])).Decode(&fs); err != nil {
		return nil, err
	}
	return fs, nil
}

func (m model) anyLoading() bool {
	return m.factsState == loading || m.pocState == loading || m.kleeState == loading
}

func main() {
	if _, err := tea.NewProgram(initialModel(), tea.WithAltScreen()).Run(); err != nil {
		fmt.Fprintln(os.Stderr, "ошибка TUI:", err)
		os.Exit(1)
	}
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return s[:i]
	}
	return s
}

func copyCmd(text string) tea.Cmd {
	return func() tea.Msg { _ = clipboard.WriteAll(text); return copiedMsg{} }
}
