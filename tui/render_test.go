package main

import (
	"regexp"
	"testing"

	tea "github.com/charmbracelet/bubbletea"
)

// рендерим экраны и печатаем БЕЗ ANSI — чтобы глазами проверить раскладку/выравнивание.
func TestRender(t *testing.T) {
	strip := regexp.MustCompile("\x1b\\[[0-9;]*m")
	m := initialModel()
	nm, _ := m.Update(tea.WindowSizeMsg{Width: 92, Height: 24})
	m = nm.(model)

	t.Log("\n=== INPUT ===\n" + strip.ReplaceAllString(m.View(), ""))

	m.findings = []finding{
		{Rule: "reentrancy", Severity: "HIGH", Function: "withdraw", Line: 6, PriorityScore: 340},
		{Rule: "access-control", Severity: "HIGH", Function: "setOwner", Line: 25, PriorityScore: 350},
		{Rule: "tx-origin", Severity: "MEDIUM", Function: "drain", Line: 31, PriorityScore: 215},
		{Rule: "unchecked-call", Severity: "LOW", Function: "poke", Line: 38, PriorityScore: 105},
	}
	m.scanned, m.state, m.cursor = "SampleVault.sol", stateList, 1
	t.Log("\n=== LIST ===\n" + strip.ReplaceAllString(m.View(), ""))

	m.detailTab, m.state = tabOverview, stateDetail
	m = m.withDetail()
	t.Log("\n=== DETAIL ===\n" + strip.ReplaceAllString(m.View(), ""))
}
