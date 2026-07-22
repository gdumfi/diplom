package main

import (
	"path/filepath"
	"strings"

	"github.com/charmbracelet/bubbles/spinner"
	"github.com/charmbracelet/bubbles/textinput"
	"github.com/charmbracelet/bubbles/viewport"
	tea "github.com/charmbracelet/bubbletea"
)

func (m model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.w, m.h = msg.Width, msg.Height
		h := msg.Height - 9
		if h < 3 {
			h = 3
		}
		m.vp = viewport.New(msg.Width-4, h)
		if m.state == stateDetail {
			m = m.withDetail()
		}
		return m, nil

	case scanDoneMsg:
		if msg.err != nil {
			m.err, m.state = msg.err, stateInput
			return m, nil
		}
		m.findings, m.scanned, m.err = msg.findings, msg.target, nil
		m.cursor, m.state = 0, stateList
		// сбрасываем кеши вкладок под новую цель
		m.facts, m.factsState = "", notLoaded
		m.poc, m.pocState = "", notLoaded
		m.klee, m.kleeState = "", notLoaded
		return m, nil

	case factsMsg:
		m.facts, m.factsState = msg.text, loaded
		if msg.err != nil {
			m.facts, m.factsState = msg.err.Error(), failed
		}
		if m.state == stateDetail && m.detailTab == tabFacts {
			m = m.withDetail()
		}
		return m, nil

	case pocMsg:
		m.poc, m.pocState = msg.text, loaded
		if msg.err != nil {
			m.poc, m.pocState = msg.err.Error(), failed
		}
		if m.state == stateDetail && m.detailTab == tabPoc {
			m = m.withDetail()
		}
		return m, nil

	case kleeMsg:
		m.klee, m.kleeState = msg.text, loaded
		if msg.err != nil && strings.TrimSpace(msg.text) == "" {
			m.klee, m.kleeState = msg.err.Error(), failed
		}
		if m.state == stateDetail && m.detailTab == tabConfirm {
			m = m.withDetail()
		}
		return m, nil

	case copiedMsg:
		m.toast = "✓ скопировано в буфер"
		return m, nil

	case spinner.TickMsg:
		var cmd tea.Cmd
		m.spin, cmd = m.spin.Update(msg)
		if m.state == stateScanning || m.anyLoading() {
			return m, cmd
		}
		return m, nil

	case tea.KeyMsg:
		switch m.state {
		case stateInput:
			return m.updateInput(msg)
		case stateList:
			return m.updateList(msg)
		case stateDetail:
			return m.updateDetail(msg)
		case stateScanning:
			if msg.String() == "ctrl+c" {
				return m, tea.Quit
			}
		}
	}
	return m, nil
}

func (m model) updateInput(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "ctrl+c", "esc":
		return m, tea.Quit
	case "tab", "shift+tab":
		m.focus = 1 - m.focus
		if m.focus == 0 {
			m.target.Focus()
			m.chain.Blur()
		} else {
			m.chain.Focus()
			m.target.Blur()
		}
		return m, textinput.Blink
	case "enter":
		target := strings.TrimSpace(m.target.Value())
		if target == "" {
			m.err = errStr("введите адрес 0x… или путь к .sol")
			return m, nil
		}
		m.isFile = !strings.HasPrefix(target, "0x")
		if m.isFile { // абсолютный путь — PoC/KLEE запускаются из временной папки
			if abs, e := filepath.Abs(target); e == nil {
				target = abs
			}
		}
		m.reqTarget = target
		m.reqChain = strings.TrimSpace(m.chain.Value())
		m.err, m.state = nil, stateScanning
		return m, tea.Batch(m.spin.Tick, m.scanCmd())
	}
	var cmd tea.Cmd
	if m.focus == 0 {
		m.target, cmd = m.target.Update(msg)
	} else {
		m.chain, cmd = m.chain.Update(msg)
	}
	return m, cmd
}

func (m model) updateList(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "ctrl+c", "q":
		return m, tea.Quit
	case "esc":
		m.state = stateInput
		return m, textinput.Blink
	case "j", "down":
		if m.cursor < len(m.findings)-1 {
			m.cursor++
		}
	case "k", "up":
		if m.cursor > 0 {
			m.cursor--
		}
	case "r":
		m.state = stateScanning
		return m, tea.Batch(m.spin.Tick, m.scanCmd())
	case "enter":
		if len(m.findings) > 0 {
			m.detailTab, m.toast = tabOverview, ""
			m = m.withDetail()
			m.state = stateDetail
		}
	}
	return m, nil
}

func (m model) updateDetail(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "ctrl+c", "q":
		return m, tea.Quit
	case "esc", "backspace":
		m.state, m.toast = stateList, ""
		return m, nil
	case "tab", "right", "l":
		return m.setTab(m.detailTab + 1)
	case "shift+tab", "left":
		return m.setTab(m.detailTab - 1)
	case "1":
		return m.setTab(tabOverview)
	case "2":
		return m.setTab(tabFacts)
	case "3":
		return m.setTab(tabPoc)
	case "4":
		return m.setTab(tabConfirm)
	case "y":
		return m, copyCmd(m.currentTabText())
	case "enter":
		if m.detailTab == tabConfirm {
			if !m.isFile {
				m.klee, m.kleeState = "KLEE-confirm доступен только для файловых целей (.sol).\nДля адреса сохрани исходник локально и просканируй файлом.", failed
				return m.withDetail(), nil
			}
			if m.kleeState == loading {
				return m, nil
			}
			fn := ""
			if m.cursor < len(m.findings) {
				fn = m.findings[m.cursor].Function
			}
			m.kleeState = loading
			m = m.withDetail()
			return m, tea.Batch(m.spin.Tick, m.kleeCmd(fn))
		}
	}
	var cmd tea.Cmd
	m.vp, cmd = m.vp.Update(msg)
	return m, cmd
}

func (m model) setTab(t int) (tea.Model, tea.Cmd) {
	if t < tabOverview {
		t = tabOverview
	}
	if t > tabConfirm {
		t = tabConfirm
	}
	m.detailTab, m.toast = t, ""
	m = m.withDetail()
	// ленивая подгрузка facts/poc при открытии вкладки
	switch t {
	case tabFacts:
		if m.factsState == notLoaded {
			m.factsState = loading
			m = m.withDetail()
			return m, tea.Batch(m.spin.Tick, m.factsCmd())
		}
	case tabPoc:
		if m.pocState == notLoaded {
			m.pocState = loading
			m = m.withDetail()
			return m, tea.Batch(m.spin.Tick, m.pocCmd())
		}
	}
	return m, nil
}

func (m model) currentTabText() string {
	switch m.detailTab {
	case tabFacts:
		return m.facts
	case tabPoc:
		return m.poc
	case tabConfirm:
		return m.klee
	default:
		if m.cursor < len(m.findings) {
			return m.findings[m.cursor].Message
		}
	}
	return ""
}

// withDetail — пересобрать содержимое viewport под текущую вкладку
func (m model) withDetail() model {
	var c string
	switch m.detailTab {
	case tabOverview:
		if m.cursor < len(m.findings) {
			c = detailBody(m.findings[m.cursor], m.w)
		}
	case tabFacts:
		c = loadView(m.factsState, m.facts, m.spin.View()+" загружаю IR-facts…", "нажми, чтобы вкладка открылась")
	case tabPoc:
		c = loadView(m.pocState, m.poc, m.spin.View()+" генерирую PoC…", "нажми, чтобы вкладка открылась")
	case tabConfirm:
		c = confirmView(m)
	}
	m.vp.SetContent(c)
	m.vp.GotoTop()
	return m
}

type strErr string

func (e strErr) Error() string { return string(e) }
func errStr(s string) error    { return strErr(s) }
