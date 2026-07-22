package main

import (
	"strings"

	"github.com/charmbracelet/lipgloss"
)

// палитра — как в витрине (web/src/index.css): primary #e6b34a (жёлто-оранжевый)
var (
	cAccent = lipgloss.Color("#e6b34a") // primary — золото
	cAccFg  = lipgloss.Color("#1a1206") // текст на золоте
	cHigh   = lipgloss.Color("#e5484d") // HIGH — красный
	cMed    = lipgloss.Color("#e4a13c") // MEDIUM — оранжевый (warning)
	cLow    = lipgloss.Color("#9a9488") // LOW — приглушённый
	cText   = lipgloss.Color("#e8e2d6") // основной текст
	cDim    = lipgloss.Color("#7a7266") // вторичный
	cOk     = lipgloss.Color("#3fb950") // успех/подтверждено
)

var (
	stTitle  = lipgloss.NewStyle().Foreground(cAccFg).Background(cAccent).Bold(true).Padding(0, 1)
	stSub    = lipgloss.NewStyle().Foreground(cDim)
	stAccent = lipgloss.NewStyle().Foreground(cAccent).Bold(true)
	stText   = lipgloss.NewStyle().Foreground(cText)
	stDim    = lipgloss.NewStyle().Foreground(cDim)
	stHelp   = lipgloss.NewStyle().Foreground(cDim)
	stCursor = lipgloss.NewStyle().Foreground(cAccent).Bold(true)
	stBox    = lipgloss.NewStyle().Border(lipgloss.RoundedBorder()).BorderForeground(cDim).Padding(0, 1)
	stSelBox = lipgloss.NewStyle().Border(lipgloss.RoundedBorder()).BorderForeground(cAccent).Padding(0, 1)
	stErr    = lipgloss.NewStyle().Foreground(cHigh).Bold(true)
	stOk     = lipgloss.NewStyle().Foreground(cOk).Bold(true)
)

// цвет/подпись по severity
func sevColor(sev string) lipgloss.Color {
	switch sev {
	case "HIGH":
		return cHigh
	case "MEDIUM":
		return cMed
	default:
		return cLow
	}
}

func sevLabel(sev string) string {
	if sev == "MEDIUM" {
		return "MED"
	}
	return sev
}

// ── «красивый» слой ──
var (
	cWhite = lipgloss.Color("#f7f2e6")
	cSelBg = lipgloss.Color("#2a2213")

	stCard   = lipgloss.NewStyle().Border(lipgloss.RoundedBorder()).BorderForeground(cDim).Padding(0, 1)
	stColHdr = lipgloss.NewStyle().Foreground(cDim).Bold(true)
)

// залитый бейдж severity (белый текст на цветном фоне)
func sevBadge(sev string) string {
	return lipgloss.NewStyle().Background(sevColor(sev)).Foreground(cWhite).Bold(true).Padding(0, 1).Render(sevLabel(sev))
}

// горизонтальная линия во всю ширину
func rule(w int) string {
	if w < 1 {
		w = 1
	}
	return lipgloss.NewStyle().Foreground(cDim).Render(strings.Repeat("─", w))
}

// pad/truncate строку до ширины n (для колонок)
func cell(s string, n int) string {
	return lipgloss.NewStyle().Width(n).MaxWidth(n).Render(s)
}

// статус находки — как в вебе (data.ts)
func statusOf(f finding) string {
	if f.SymConfirmed {
		return "SYM-CONFIRMED"
	}
	if f.Severity == "LOW" {
		return "info"
	}
	return "candidate"
}
