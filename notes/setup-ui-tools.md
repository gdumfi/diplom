# Инструкция: поставить UI-инструменты в проект (для другого Claude Code)

> Отдай этот файл (или его текст) другому агенту. Ставятся ДВА инструмента:
> **shadcn MCP** (проектный) и **Stitch-скиллы** (пользовательские).
> Проверено на рабочей конфигурации: `~/diplom` (React 19 + Vite + Tailwind 4 + shadcn).

---

## Что именно ставим

| Инструмент | Что это | Область | Где живёт |
|---|---|---|---|
| **shadcn MCP** | поиск/просмотр/установка готовых компонентов из реестров | **проект** | `.mcp.json` в корне проекта |
| **Stitch-скиллы** | 14 скиллов: генерация дизайна, извлечение дизайн-системы, Stitch↔React | **пользователь** | `~/.claude/skills/<имя>/` |

**Предусловия:** установлен Node.js + `npx` (нужен для shadcn MCP).

---

## Шаг 1. shadcn MCP (проектный)

Создать в **корне проекта** файл `.mcp.json`:

```json
{
  "mcpServers": {
    "shadcn": {
      "command": "npx",
      "args": ["shadcn@latest", "mcp"]
    }
  }
}
```

Эквивалент через CLI:
```bash
claude mcp add shadcn -- npx shadcn@latest mcp
```

Если проект ещё не инициализирован под shadcn (нет `components.json`) — сначала:
```bash
npx shadcn@latest init
```
Без `components.json` инструмент `get_add_command_for_items` не сможет собрать команду установки.

**Проверка:** перезапустить сессию → `/mcp` должен показать сервер `shadcn`.
Появятся инструменты: `search_items_in_registries`, `list_items_in_registries`,
`view_items_in_registries`, `get_add_command_for_items`, `get_item_examples_from_registries`,
`get_audit_checklist`, `get_project_registries`.

---

## Шаг 2. Stitch-скиллы (пользовательские)

**Важно:** это скиллы **пользовательской области** (`~/.claude/skills/`) — они автоматически
доступны во ВСЕХ проектах этого пользователя. Ставить их «в проект» не нужно.

### Если это та же машина/пользователь
**Ничего делать не надо** — скиллы уже доступны в любом проекте. Переходи к шагу 1.

### Если другая машина/пользователь
Скопировать 14 папок в `~/.claude/skills/`:

```
code-to-design      design-md            enhance-prompt      extract-design-md
extract-static-html generate-design      manage-design-system react-components
react-native        remotion             shadcn-ui           stitch-loop
taste-design        upload-to-stitch
```

Перенос с рабочей машины:
```bash
# на источнике
tar -czf stitch-skills.tgz -C ~/.claude/skills .
# на приёмнике
mkdir -p ~/.claude/skills && tar -xzf stitch-skills.tgz -C ~/.claude/skills
```
Каждый скилл — папка с `SKILL.md` (+ `examples/`, `references/`).
Это НЕ git-репозиторий и НЕ плагин — просто каталоги.

**Проверка:** перезапустить сессию → скиллы видны как `/generate-design`, `/extract-design-md` и т.д.

---

## ⚠️ Честное ограничение (обязательно прочитать)

Скиллы Stitch делятся на две группы:

**(A) Требуют Stitch MCP-сервер** — в их frontmatter `allowed-tools: "stitch*:*"`, они ходят
в Stitch через MCP-инструменты `stitch*` и требуют **API-ключ Stitch**:
`generate-design`, `upload-to-stitch`, `manage-design-system`, `code-to-design`, `stitch-loop`.

> **На эталонной конфигурации Stitch-MCP НЕ подключён** — в `.mcp.json` есть только `shadcn`.
> Значит эти скиллы «установлены», но без отдельно настроенного Stitch-MCP + ключа **работать
> не будут**. Если нужна генерация дизайна — придётся дополнительно поднять Stitch MCP-сервер
> и прописать его в `.mcp.json` рядом с shadcn.

**(B) Работают локально, без Stitch** (по коду проекта, ничего настраивать не надо):
`extract-design-md`, `extract-static-html`, `design-md`, `taste-design`, `react-components`,
`react-native`, `enhance-prompt`, `remotion`, `shadcn-ui`.

**Практический вывод:** рабочая связка «из коробки» = **shadcn MCP** (ставить компоненты) +
**группа (B)** (извлекать/держать дизайн-систему по коду). Генерация дизайна в Stitch требует
дополнительной настройки, которой на эталоне нет.

---

## Итоговый чек-лист

1. [ ] Node.js + npx есть
2. [ ] `.mcp.json` с сервером `shadcn` в корне проекта
3. [ ] `npx shadcn@latest init` (если нет `components.json`)
4. [ ] Скиллы: та же машина → уже есть; другая → скопировать 14 папок в `~/.claude/skills/`
5. [ ] Перезапуск сессии; `/mcp` показывает `shadcn`; скиллы видны
6. [ ] Помнить: Stitch-генерация без отдельного Stitch-MCP + ключа не заработает
