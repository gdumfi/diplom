# SCSA TUI

Терминальный интерфейс сканера (Go + Bubble Tea). Тонкая «морда» по AD-5: дёргает бинарь
`sca --audit --json` и рисует находки. Тема — под витрину (жёлто-оранжевый `#e6b34a`).

## Экраны
ввод цели (адрес/файл + сеть) → скан (спиннер) → список находок (раскраска по severity, `j/k`,
`enter`) → **деталь с вкладками**:
- **Overview** — суть находки (`message`), функция/строка/score, storage-слот.
- **IR-facts** — `sca --llm-context` (ленивая подгрузка, скролл).
- **PoC** — `sca --poc` (генерит `*PoC.t.sol` во временной папке, показывает).
- **Confirm** — `enter` запускает KLEE (`run_klee.sh`, docker; только для файловых целей).

Клавиши: `←/→` вкладки · `j/k` прокрутка/выбор · `y` копировать вкладку в буфер · `esc` назад · `q` выход.

## Сборка
```bash
export PATH=$HOME/go-sdk/go/bin:$PATH GOSUMDB=off
cd ~/diplom/tui && go build -o scsa-tui .
```
(нужен Go ≥ 1.24 — Charm-библиотеки; локально в `~/go-sdk`)

## Запуск
```bash
# через бинарь (рекомендуется) — он сам передаёт себя как SCA_BIN:
scsa-tui   # положи рядом в PATH, тогда:
sca tui    # ← подкоманда бинаря: execs scsa-tui

# напрямую (укажи, какой анализатор звать):
SCA_BIN=~/diplom/solidity-llvm/build/src/solidity-llvm ./scsa-tui
```
Для скана по адресу нужен `ETHERSCAN_API_KEY` в окружении (пробрасывается в бинарь).

## Файлы
- `main.go` — модель Bubble Tea (состояния, скан через `exec`, парсинг `--json`)
- `view.go` — рендер экранов
- `theme.go` — палитра/стили (цвета витрины) + маппинг severity/статус

## TODO (Phase 3, см. `notes/PLAN-cli-tui.md`)
таб IR-facts (`--llm-context`), Confirm (`run_klee.sh`), PoC (`sca --poc`), hunt-queue, copy.
