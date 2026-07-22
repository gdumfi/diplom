package main

import (
	"fmt"
	"strings"

	"github.com/charmbracelet/lipgloss"
)

func (m model) View() string {
	switch m.state {
	case stateInput:
		return m.viewInput()
	case stateScanning:
		return m.viewScanning()
	case stateList:
		return m.viewList()
	case stateDetail:
		return m.viewDetail()
	}
	return ""
}

// единая рамка: хедер + линия · контент (на всю высоту) · линия + подсказки.
func (m model) frame(body, help string) string {
	w := m.w
	if w <= 0 {
		w = 80
	}
	h := m.h
	if h <= 0 {
		h = 24
	}
	header := " " + stAccent.Render("▚ SCSA") + stDim.Render("   security scanner") + "\n" + rule(w)
	footer := rule(w) + "\n " + stDim.Render(help)
	fill := h - lipgloss.Height(header) - lipgloss.Height(footer)
	if fill < 1 {
		fill = 1
	}
	inner := lipgloss.NewStyle().Width(w).Padding(1, 2).Height(fill).MaxHeight(fill).Render(body)
	return lipgloss.JoinVertical(lipgloss.Left, header, inner, footer)
}

func (m model) viewInput() string {
	var b strings.Builder
	b.WriteString(stText.Bold(true).Render("Наведи сканер на цель") + "\n")
	b.WriteString(stDim.Render("верифицированный адрес или локальный .sol · pre-0.8 пропускается") + "\n\n")

	tBox, cBox := stBox, stBox
	if m.focus == 0 {
		tBox = stSelBox
	} else {
		cBox = stSelBox
	}
	b.WriteString(stDim.Render("ЦЕЛЬ") + "\n")
	b.WriteString(tBox.Width(48).Render(m.target.View()) + "\n\n")
	b.WriteString(stDim.Render("СЕТЬ") + stDim.Render("   1=ETH · 8453=Base · 42161=Arbitrum") + "\n")
	b.WriteString(cBox.Width(12).Render(m.chain.View()))
	if m.err != nil {
		b.WriteString("\n\n" + stErr.Render("⚠ "+m.err.Error()))
	}
	return m.frame(b.String(), "tab переключить · enter сканировать · esc выход")
}

func (m model) viewScanning() string {
	steps := strings.Join([]string{"lower → LLVM IR", "CFG-детекторы", "триаж", "JSON"}, stDim.Render("  →  "))
	body := m.spin.View() + " " + stText.Render("сканирую ") + stAccent.Render(m.reqTarget) + "\n\n" +
		stDim.Render(steps)
	return m.frame(body, "ctrl+c прервать")
}

func countChip(label string, n int, c lipgloss.Color) string {
	return lipgloss.NewStyle().Foreground(c).Bold(true).Render(fmt.Sprintf("%d", n)) + stDim.Render(" "+label)
}

func (m model) viewList() string {
	var b strings.Builder
	b.WriteString(stAccent.Render(m.scanned) + stDim.Render(fmt.Sprintf("   %d находок", len(m.findings))) + "\n")
	b.WriteString(countChip("HIGH", countSev(m.findings, "HIGH"), cHigh) + "    " +
		countChip("MED", countSev(m.findings, "MEDIUM"), cMed) + "    " +
		countChip("LOW", countSev(m.findings, "LOW"), cLow) + "\n\n")

	if len(m.findings) == 0 {
		b.WriteString(stOk.Render("✓ чисто") + stDim.Render("  — по классам SCSA находок нет"))
		return m.frame(b.String(), "r пересканировать · esc назад · q выход")
	}
	b.WriteString(m.findingsTable())
	return m.frame(b.String(), "j/k выбор · enter деталь · r пересканировать · esc назад · q выход")
}

func (m model) findingsTable() string {
	iw := m.w - 8
	if iw < 44 {
		iw = 44
	}
	const badgeW, clsW = 9, 22
	var rows []string
	rows = append(rows, "  "+stColHdr.Render(cell("SEV", badgeW)+cell("CLASS", clsW)+"LOCATION"))
	for i, f := range m.findings {
		badge := cell(sevBadge(f.Severity), badgeW)
		cls := cell(truncate(f.Rule, clsW-2), clsW)
		loc := fmt.Sprintf("%s()  L%d   %s", f.Function, f.Line, statusOf(f))
		prefix := "  "
		var line string
		if i == m.cursor {
			prefix = stCursor.Render("▌ ")
			line = badge + lipgloss.NewStyle().Foreground(cText).Bold(true).Render(cls) + lipgloss.NewStyle().Foreground(cText).Render(loc)
		} else {
			line = badge + lipgloss.NewStyle().Foreground(cText).Render(cls) + stDim.Render(loc)
		}
		rows = append(rows, prefix+line)
	}
	return stCard.Width(iw).Render(strings.Join(rows, "\n"))
}

func detailBody(f finding, w int) string {
	if w <= 0 {
		w = 80
	}
	wrapW := w - 10
	if wrapW < 20 {
		wrapW = 20
	}
	var b strings.Builder
	b.WriteString(sevBadge(f.Severity) + "  " + stAccent.Bold(true).Render(f.Rule) + "\n")
	b.WriteString(stDim.Render(fmt.Sprintf("%s()   строка %d   score %d   %s", f.Function, f.Line, f.PriorityScore, statusOf(f))) + "\n\n")
	b.WriteString(stText.Bold(true).Render("Суть") + "\n")
	b.WriteString(stText.Render(wrap(f.Message, wrapW)))
	if f.StorageSlot >= 0 {
		b.WriteString("\n\n" + stDim.Render(fmt.Sprintf("storage-слот  %d", f.StorageSlot)))
	}
	return b.String()
}

func (m model) viewDetail() string {
	var b strings.Builder
	if m.toast != "" {
		b.WriteString(stOk.Render(m.toast) + "\n\n")
	}
	b.WriteString(tabBar(m.detailTab) + "\n")
	b.WriteString(rule(m.w-4) + "\n\n")
	b.WriteString(m.vp.View())
	help := "←/→ вкладки · j/k прокрутка · y копировать · esc назад · q выход"
	if m.detailTab == tabConfirm {
		help = "enter запустить KLEE · ←/→ вкладки · y копировать · esc назад"
	}
	return m.frame(b.String(), help)
}

func tabBar(active int) string {
	parts := make([]string, len(tabNames))
	for i, n := range tabNames {
		if i == active {
			parts[i] = lipgloss.NewStyle().Foreground(cAccFg).Background(cAccent).Bold(true).Padding(0, 1).Render(n)
		} else {
			parts[i] = lipgloss.NewStyle().Foreground(cDim).Padding(0, 1).Render(n)
		}
	}
	return strings.Join(parts, stDim.Render("·"))
}

// экран вкладки с ленивой загрузкой (facts/poc)
func loadView(s loadState, content, loadingMsg, hint string) string {
	switch s {
	case loading:
		return loadingMsg
	case loaded:
		return content
	case failed:
		return stErr.Render(content)
	default:
		return stDim.Render(hint)
	}
}

// экран вкладки Confirm (KLEE)
func confirmView(m model) string {
	switch m.kleeState {
	case loading:
		return m.spin.View() + " KLEE символьно исследует путь… (может занять минуты)\n\n" +
			stDim.Render("docker klee/klee · fork reentrancy глубины 1 · оракул total_sent>storage")
	case loaded:
		return m.klee
	case failed:
		return stErr.Render(m.klee)
	default:
		fn := ""
		if m.cursor < len(m.findings) {
			fn = m.findings[m.cursor].Function
		}
		return stText.Bold(true).Render("KLEE-подтверждение") + "\n\n" +
			stDim.Render("Символьно проверяет достижимость эксплойта для ") + stAccent.Render(fn+"()") + "\n" +
			stDim.Render("через run_klee.sh (docker klee). Только для файловых целей.") + "\n\n" +
			stAccent.Bold(true).Render("[enter] запустить")
	}
}

func countSev(fs []finding, sev string) int {
	n := 0
	for _, f := range fs {
		if f.Severity == sev {
			n++
		}
	}
	return n
}

func truncate(s string, n int) string {
	if lipgloss.Width(s) <= n {
		return s
	}
	if n <= 1 {
		return string([]rune(s)[:n])
	}
	r := []rune(s)
	if len(r) > n-1 {
		r = r[:n-1]
	}
	return string(r) + "…"
}

func wrap(s string, w int) string {
	words := strings.Fields(s)
	var lines []string
	cur := ""
	for _, wd := range words {
		if len(cur)+len(wd)+1 > w {
			lines = append(lines, cur)
			cur = wd
		} else if cur == "" {
			cur = wd
		} else {
			cur += " " + wd
		}
	}
	if cur != "" {
		lines = append(lines, cur)
	}
	return strings.Join(lines, "\n")
}
