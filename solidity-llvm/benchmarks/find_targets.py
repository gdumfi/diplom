#!/usr/bin/env python3
# find_targets.py — автопоиск СВЕЖИХ целей на аудит-контест-площадках (где EV тула положителен:
# неаудированный код, который платит). Источники с рабочим JSON-API: Sherlock, Cantina.
# Code4rena закрывается (SPA без API) → best-effort пропускаем.
#
# зачем: контест-код — свежий и оплачиваемый, в отличие от вылизанных mainnet-протоколов, где
# recall тула ≈0. это верхняя воронка автономного хант-цикла (HUNT-LOOP-PLAN §S0).
#
# вывод (в ~/diplom/hunt/):
#   contests.md   — ранжированная человекочитаемая таблица активных контестов
#   targets.json  — machine-readable: репо+commit+in-scope файлы (для fetch_scope.py → hunt_batch)
#
# зависимости: только stdlib. сеть ненадёжна → таймауты + ретраи + мягкая деградация по площадке.

import json
import os
import sys
import time
import urllib.request
from datetime import datetime, timezone

HUNT_DIR = os.path.expanduser("~/diplom/hunt")

SHERLOCK_LIST = "https://mainnet-contest.sherlock.xyz/contests"
SHERLOCK_DETAIL = "https://mainnet-contest.sherlock.xyz/contests/{id}"
CANTINA_LIST = "https://cantina.xyz/api/v0/competitions"

# статусы активных контестов по площадкам (что ещё принимает сабмиты)
SHERLOCK_ACTIVE = {"RUNNING"}
CANTINA_ACTIVE = {"active", "live", "open", "running"}


def get_json(url, tries=4, timeout=30):
    """GET → JSON с ретраями; None при неудаче (сеть ненадёжна — деградируем мягко)."""
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "scsa-hunt/0.1"})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return json.loads(r.read())
        except Exception as e:  # noqa: BLE001
            sys.stderr.write(f"  retry {i+1}/{tries} {url}: {e}\n")
            time.sleep(2)
    return None


def ts_to_date(ts):
    if not ts:
        return "?"
    try:
        return datetime.fromtimestamp(int(ts), tz=timezone.utc).strftime("%Y-%m-%d")
    except Exception:  # noqa: BLE001
        return "?"


def fetch_sherlock():
    """активные публичные Sherlock-контесты + их scope (репо/commit/файлы). список целей."""
    targets = []
    data = get_json(SHERLOCK_LIST)
    if not data:
        sys.stderr.write("Sherlock: список недоступен\n")
        return targets
    items = data.get("items", [])
    # добираем несколько страниц (активные могут быть не только на первой)
    page = 2
    while data and data.get("next_page") and page <= 3:
        nxt = get_json(f"{SHERLOCK_LIST}?page={page}")
        if not nxt:
            break
        items += nxt.get("items", [])
        data = nxt
        page += 1

    for it in items:
        if it.get("status") not in SHERLOCK_ACTIVE or it.get("private"):
            continue
        cid = it.get("id")
        det = get_json(SHERLOCK_DETAIL.format(id=cid)) or {}
        scope = det.get("scope") or []
        repos = []
        files_total = 0
        for s in scope:
            files = [f.get("name") for f in (s.get("files") or []) if f.get("name")]
            files_total += len(files)
            repos.append({
                "repo": s.get("repo"),               # Owner/name на GitHub
                "branch": s.get("branch_name") or "main",
                "commit": s.get("commit_hash") or "",
                "nsloc": s.get("total_nsloc"),
                "files": files,                      # in-scope пути (могут быть .sol и не только)
            })
        targets.append({
            "platform": "sherlock",
            "id": cid,
            "title": it.get("title"),
            "prize": it.get("rewards") or it.get("prize_pool"),
            "token": it.get("token"),
            "kyc": bool(det.get("requires_kyc")),
            "ends_at": it.get("ends_at"),
            "nsloc": det.get("nsloc") or it.get("nsloc"),
            "audit_repo": det.get("template_repo_name"),   # sherlock-audit/... (зеркало со скоупом)
            "scope_repos": repos,
            "url": f"https://audits.sherlock.xyz/contests/{cid}",
            "desc": (it.get("short_description") or "")[:200],
        })
    return targets


def fetch_cantina():
    """активные Cantina-контесты (репо в detail часто gated → отдаём листинг для ручного захода)."""
    targets = []
    data = get_json(CANTINA_LIST)
    if not data:
        sys.stderr.write("Cantina: список недоступен\n")
        return targets
    for c in (data if isinstance(data, list) else data.get("items", [])):
        st = str(c.get("status") or "").lower()
        if st not in CANTINA_ACTIVE:
            continue
        tf = c.get("timeframe") or {}
        targets.append({
            "platform": "cantina",
            "id": c.get("id"),
            "title": c.get("name"),
            "prize": c.get("totalRewardPot"),
            "token": c.get("currencyCode"),
            "kyc": bool(c.get("kycRequired")),
            "ends_at": tf.get("end") or tf.get("endDate"),
            "nsloc": None,
            "audit_repo": None,          # Cantina хостит код у себя (gated) → репо не всегда public
            "scope_repos": [],
            "url": c.get("url"),
            "desc": (c.get("instructions") or "")[:200],
        })
    return targets


def prize_num(p):
    try:
        return float(p)
    except Exception:  # noqa: BLE001
        return 0.0


def write_outputs(targets):
    os.makedirs(HUNT_DIR, exist_ok=True)
    # ранжируем: приз↓, без-KYC выше (доступнее с РФ — предпочтение пользователя)
    targets.sort(key=lambda t: (prize_num(t["prize"]), not t["kyc"]), reverse=True)

    # machine-readable — для fetch_scope.py
    with open(os.path.join(HUNT_DIR, "targets.json"), "w") as f:
        json.dump(targets, f, ensure_ascii=False, indent=1)

    # человекочитаемая таблица
    lines = [
        "# CONTESTS — активные аудит-контесты (автопоиск find_targets.py)",
        "",
        f"обновлено (UTC): {ts_to_date(int(time.time()))} · целей: {len(targets)}",
        "",
        "Приоритет: приз ↓, без-KYC выше (доступнее с РФ). Sherlock даёт public GitHub-репо +",
        "in-scope файлы → fetch_scope.py тянет их для триажа. Cantina часто gated (репо вручную).",
        "",
        "| # | площадка | контест | приз | KYC | дедлайн | nSLOC | репо (scope) |",
        "|---|----------|---------|------|-----|---------|-------|--------------|",
    ]
    for i, t in enumerate(targets, 1):
        repo = t["scope_repos"][0]["repo"] if t["scope_repos"] else (t["audit_repo"] or t["url"])
        prize = f"{prize_num(t['prize']):,.0f} {t['token'] or ''}".strip()
        kyc = "⚠️KYC" if t["kyc"] else "нет"
        lines.append(
            f"| {i} | {t['platform']} | {t['title']} | {prize} | {kyc} "
            f"| {ts_to_date(t['ends_at'])} | {t['nsloc'] or '?'} | {repo} |"
        )
    lines += ["", "## Как взять цель в работу", "",
              "1. `fetch_scope.py <#|title>` — тянет in-scope .sol из репо (Sherlock).",
              "2. `hunt_batch.sh --dir hunt/repos/<contest>` — дешёвый триаж → hunt_queue.md.",
              "3. `/hunt <файл>` или хант-цикл — глубокий разбор выживших + forge-подтверждение.",
              "", "Границы: контест-скоуп строгий — читай правила контеста (out-of-scope классы,",
              "severity-модель) ПЕРЕД сабмитом. KYC-контесты помечены (предпочтение — без KYC)."]
    with open(os.path.join(HUNT_DIR, "contests.md"), "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    all_targets = []
    sys.stderr.write("Sherlock…\n"); all_targets += fetch_sherlock()
    sys.stderr.write("Cantina…\n");  all_targets += fetch_cantina()
    # Code4rena закрывается (SPA без JSON-API) → пропускаем; при желании — ручной ввод в seed.txt
    write_outputs(all_targets)
    print(f"найдено активных контестов: {len(all_targets)} → {HUNT_DIR}/contests.md, targets.json")
    for t in all_targets[:10]:
        print(f"  [{t['platform']}] {t['title']} — {prize_num(t['prize']):,.0f} {t['token'] or ''}"
              f"{' KYC' if t['kyc'] else ''}")


if __name__ == "__main__":
    main()
