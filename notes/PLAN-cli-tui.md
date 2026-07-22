# План: CLI-инструкция + TUI

> Цель: (A) кнопка «or run the CLI» ведёт на реальную инструкцию установки/запуска CLI;
> (B) у инструмента есть красивый TUI (терминальный интерфейс). Опирается на контекст-аудит ниже.

---

## 0. Контекст-аудит (что есть / чего нет — по коду)

| Что | Состояние |
|---|---|
| Кнопка «or run the CLI» | [Landing.tsx:152] — `<a href="#features">`, **мёртвая** (скроллит к фичам, инструкции нет) |
| «Install the CLI» на Pricing | ведёт на `/app` — тоже не инструкция |
| Бинарь `sca` (`solidity-llvm`) | cxxopts; режимы — ФЛАГАМИ (`--input/--address/--audit/--json/--sarif/--llm-context/--poc/--emit-klee/--emit-invariant`), без subcommand |
| Интерактив / TUI | **нет** (ни ftxui, ни ncurses, ни readline) |
| Дистрибуция бинаря | 59 МБ, динамич. линковка с LLVM-18 + в рантайме зовёт `docker` (solc) → «установка» реалистично только через docker |
| Контракт вывода | `--json` = массив `{rule,severity,function,line,storageSlot,symConfirmed,priorityScore,message}`; `--llm-context` = markdown-пак фактов |
| Go-модели JSON | уже написаны в `web/server/main.go` (toolFinding→finding) — **TUI переиспользует** |
| AD-5 | анализ только в бинаре; TUI/веб — тонкие фронты, дёргают `sca` + парсят JSON |

**Вывод:** обе задачи ложатся на существующий `--json`-контракт. TUI не переписывает анализ —
он вторая «морда» рядом с вебом, поверх того же вывода бинаря.

---

## A. «or run the CLI» → инструкция установки

### A1. Куда ведёт кнопка
Новый роут `/cli` в SPA (страница `pages/InstallCli.tsx`). Кнопку в `Landing.tsx` и «Install the CLI»
в `Pricing.tsx` перевести на `<Link to="/cli">`.

### A2. Что на странице (copy-paste блоки)
- **Установка (docker-путь, рекомендуемый).** Бинарь тянет LLVM+docker, поэтому раздаём образ + тонкий
  враппер `sca`:
  ```bash
  # 1) образ анализатора (собрать локально ИЛИ docker pull, когда опубликуем в реестр)
  git clone <repo> && cd solidity-llvm && docker build -t scsa .
  # 2) враппер sca → в PATH
  curl -fsSL <install-url>/sca > ~/.local/bin/sca && chmod +x ~/.local/bin/sca
  ```
  Враппер `sca` = обёртка над `docker run --rm -v $PWD:/work -v /var/run/docker.sock:/var/run/docker.sock -e ETHERSCAN_API_KEY scsa "$@"`.
- **Быстрый старт (команды):**
  ```bash
  sca -i Vault.sol --audit                 # аудит файла
  sca --address 0x… --chain 1 --audit      # аудит по адресу (нужен ETHERSCAN_API_KEY)
  sca -i Vault.sol --audit --json          # машинный вывод
  sca -i Vault.sol --poc                   # сгенерить Foundry-PoC
  sca                                       # ← запустить TUI (после Part B)
  ```
- **Установка ключа Etherscan** (для `--address`).
- Ссылка на `КАК-ПОЛЬЗОВАТЬСЯ.md` / плейбук.

### A3. Артефакты дистрибуции (Phase D ниже)
- `sca` враппер-скрипт (bash).
- `install.sh` (`curl … | sh`) — ставит враппер + проверяет docker.
- Опубликованный образ `ghcr.io/<user>/scsa` (чтобы не собирать локально).

**Оценка A:** страница + враппер — пол-дня. Публикация образа — отдельно (Phase D).

---

## B. TUI — терминальный интерфейс

### B1. Решение по архитектуре (главное)

Два варианта:

| | Вариант 1 — **Go + Bubble Tea** (обёртка над `sca --json`) | Вариант 2 — FTXUI внутри C++-бинаря |
|---|---|---|
| Что запускает | `scsa` (или `sca tui` шеллит к нему) | сам `sca` без аргументов → TUI |
| Красота/скорость разработки | ⭐ экосистема Charm (lipgloss/bubbles) — самый красивый результат быстрее всего | C++-UI многословнее, дольше |
| Переиспользование | модели JSON из `web/server` — общий Go-пакет | нужен свой слой |
| AD-5 | идеально (фронт дёргает бинарь, как веб) | тоже ок (в процессе) |
| Дистрибуция | +1 маленький Go-бинарь (статический) | один бинарь |
| Риск | низкий (тот же `--json`, уже проверен вебом) | средний (UI-код в C++, +зависимость сборки) |

**Рекомендация: Вариант 1 (Go + Bubble Tea).** Причины: (1) ты любишь TUI → Charm даёт самый
красивый результат; (2) переиспользуем уже готовый `--json`-контракт и Go-модели из веб-бэкенда;
(3) низкий риск; (4) Go уже настроен. Чтобы ощущалось как «сам инструмент — TUI»: сделать так,
что `sca tui` (или `sca` без аргументов на TTY) запускает этот бинарь. Вариант 2 — как будущий
«пуристский один-бинарь», если захотим.

Стек: **Bubble Tea** (архитектура Elm-like) + **Lipgloss** (стили/цвета) + **Bubbles** (list/table/
spinner/textinput/viewport). Общий пакет `pkg/scsa` (модели+парсинг `--json`/`--llm-context`),
которым пользуются И веб-бэкенд, И TUI.

### B2. Экраны (зеркалят витрину в терминале)

1. **Home / цель** — табы `[a]ddress` / `[f]ile`; поле ввода адрес+сеть или пикер `.sol`; список
   недавних сканов. Enter → скан.
2. **Scanning** — спиннер + шаги пайплайна (`lower → detectors → …`), стрим статуса.
3. **Findings** — таблица, раскраска по severity (HIGH=красный, MED=янтарь, LOW=серый),
   сортировка по score, `j/k` навигация, `/` поиск, Enter → деталь. Строка-счётчик (High/Med/…).
4. **Finding detail** — rule/функция/строка/`message`; табы Overview / IR-facts (`--llm-context`) /
   Confirm / PoC. Действия: `[k]` confirm with KLEE (шеллит `run_klee.sh`), `[p]` generate PoC
   (`sca --poc`), `[y]` копировать.
5. **Hunt queue (batch)** — скормить список адресов, смотреть как заполняется очередь (как
   `hunt_batch.sh`), триаж выживших. (Опционально, Phase 3.)

Клавиши: vim-style (`j/k/enter/q/tab`, `/` поиск, `?` help), мышь опц. Хедер + статус-бар снизу.
Цвета/иконки — как в вебе (единый визуальный язык).

### B3. Поток данных
- Скан: `scsa` вызывает `sca --address … --json` (или `-i file --json`) как subprocess → парсит →
  рендерит. Тот же путь, что веб-бэкенд.
- Facts: `sca … --llm-context` → markdown → рендер в viewport.
- Confirm/PoC: шеллит `run_klee.sh` / `sca --poc`, стримит вывод в панель.
- Прогресс батча: `hunt_batch.sh` или прямые вызовы, парсинг построчно.

---

## C. Фазовый роадмап

- **Phase 1 — CLI-страница (быстрый выигрыш, web).** Роут `/cli` + `InstallCli.tsx` (docker-инструкция,
  команды, «launch the TUI» тизер). Перевесить кнопки Landing/Pricing. *~пол-дня.*
- **Phase 2 — TUI MVP (Go/Bubble Tea).** `cmd/scsa-tui`: Home(ввод) → Scanning → Findings(таблица,
  раскраска) → Detail(Overview из `message`). Общий `pkg/scsa`. *~2–3 дня.*
- **Phase 3 — TUI-глубина.** Таб IR-facts (`--llm-context`), Confirm (`run_klee.sh`), PoC (`sca --poc`),
  copy, поиск, hunt-queue экран. *~2–3 дня.*
- **Phase D — Дистрибуция.** Враппер `sca` + `install.sh` + публикация образа `ghcr.io`; `sca tui`
  запускает TUI-бинарь; обновить `/cli`. *~день.*

## D. Что строить первым
Рекомендую: **Phase 1** (страница `/cli` — кнопка перестанет быть мёртвой, сразу видимый результат),
затем **Phase 2** (TUI MVP — то, что ты любишь). Phase 3/D — по мере вкуса.

## E. Открытые вопросы (решить до старта)
1. Имя TUI-команды: `scsa` отдельно, или `sca tui`, или `sca` без аргументов на TTY? (рек.: `sca tui`).
2. Публиковать docker-образ в `ghcr.io` под твоим аккаунтом? (нужен доступ; иначе install = build локально).
3. TUI-цветовая схема — взять из витрины (тёмная, primary-акцент) 1:1?
