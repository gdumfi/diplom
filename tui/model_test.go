package main

import (
	"testing"

	tea "github.com/charmbracelet/bubbletea"
)

// детерминированная проверка машины состояний TUI (без терминала):
// ввод → скан → список → деталь → вкладки/ленивая загрузка.
func TestFlow(t *testing.T) {
	m := initialModel()

	nm, _ := m.Update(tea.WindowSizeMsg{Width: 90, Height: 30})
	m = nm.(model)

	// вводим цель и жмём enter → скан
	m.target.SetValue("/tmp/tui_vault.sol")
	nm, cmd := m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = nm.(model)
	if m.state != stateScanning {
		t.Fatalf("после enter ожидалось scanning, получено %d", m.state)
	}
	if cmd == nil {
		t.Fatal("ожидалась команда скана")
	}
	if !m.isFile {
		t.Fatal("цель-файл должна быть isFile")
	}

	// имитируем результат скана
	fs := []finding{{Rule: "reentrancy", Severity: "HIGH", Function: "withdraw", Line: 6, PriorityScore: 340, Message: "тест"}}
	nm, _ = m.Update(scanDoneMsg{findings: fs, target: "/tmp/tui_vault.sol"})
	m = nm.(model)
	if m.state != stateList || len(m.findings) != 1 {
		t.Fatalf("ожидался список с 1 находкой, state=%d n=%d", m.state, len(m.findings))
	}

	// enter → деталь
	nm, _ = m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = nm.(model)
	if m.state != stateDetail || m.detailTab != tabOverview {
		t.Fatalf("ожидалась деталь/Overview, state=%d tab=%d", m.state, m.detailTab)
	}

	// вкладка IR-facts (клавиша "2") → ленивая загрузка
	nm, cmd = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune("2")})
	m = nm.(model)
	if m.detailTab != tabFacts || m.factsState != loading || cmd == nil {
		t.Fatalf("ожидалась загрузка IR-facts, tab=%d st=%d", m.detailTab, m.factsState)
	}

	// факты пришли
	nm, _ = m.Update(factsMsg{text: "IR-facts тут"})
	m = nm.(model)
	if m.factsState != loaded || m.facts != "IR-facts тут" {
		t.Fatalf("факты не загрузились: st=%d", m.factsState)
	}

	// вкладка Confirm (клавиша "4"), enter → KLEE (file-mode) уходит в loading
	nm, _ = m.Update(tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune("4")})
	m = nm.(model)
	nm, cmd = m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = nm.(model)
	if m.detailTab != tabConfirm || m.kleeState != loading || cmd == nil {
		t.Fatalf("ожидался запуск KLEE, tab=%d st=%d", m.detailTab, m.kleeState)
	}

	// esc → назад в список
	nm, _ = m.Update(tea.KeyMsg{Type: tea.KeyEsc})
	m = nm.(model)
	if m.state != stateList {
		t.Fatalf("esc должен вернуть в список, state=%d", m.state)
	}
}

// пустой результат → «чисто», деталь недоступна
func TestCleanScan(t *testing.T) {
	m := initialModel()
	nm, _ := m.Update(scanDoneMsg{findings: nil, target: "0xabc"})
	m = nm.(model)
	if m.state != stateList || len(m.findings) != 0 {
		t.Fatalf("ожидался пустой список")
	}
	// enter не должен падать/переходить в деталь
	nm, _ = m.Update(tea.KeyMsg{Type: tea.KeyEnter})
	m = nm.(model)
	if m.state != stateList {
		t.Fatalf("на пустом списке enter не должен открывать деталь")
	}
}
