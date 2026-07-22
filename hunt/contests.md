# CONTESTS — активные аудит-контесты (автопоиск find_targets.py)

обновлено (UTC): 2026-07-21 · целей: 2

Приоритет: приз ↓, без-KYC выше (доступнее с РФ). Sherlock даёт public GitHub-репо +
in-scope файлы → fetch_scope.py тянет их для триажа. Cantina часто gated (репо вручную).

| # | площадка | контест | приз | KYC | дедлайн | nSLOC | репо (scope) |
|---|----------|---------|------|-----|---------|-------|--------------|
| 1 | sherlock | Metric | 150,000 USDC | ⚠️KYC | 2026-07-27 | 5862 | Metric-OMM/metric-core |
| 2 | sherlock | Tare | 50,000 USDC | нет | 2026-07-29 | 2542 | sherlock-scoping/tare-io__tare-contracts |

## Как взять цель в работу

1. `fetch_scope.py <#|title>` — тянет in-scope .sol из репо (Sherlock).
2. `hunt_batch.sh --dir hunt/repos/<contest>` — дешёвый триаж → hunt_queue.md.
3. `/hunt <файл>` или хант-цикл — глубокий разбор выживших + forge-подтверждение.

Границы: контест-скоуп строгий — читай правила контеста (out-of-scope классы,
severity-модель) ПЕРЕД сабмитом. KYC-контесты помечены (предпочтение — без KYC).
