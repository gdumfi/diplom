#!/usr/bin/env python3
# fetch_scope.py — тянет исходники in-scope репо контеста (из hunt/targets.json) для триажа.
# клон дохнет на sustained-коннекте → качаем мелкими запросами (GitHub tree API + raw по commit).
#
# usage: fetch_scope.py <#|подстрока-названия>
#   пример: fetch_scope.py 2   /   fetch_scope.py Tare
# вывод: hunt/repos/<title>/ с .sol; далее hunt_batch.sh --dir на этот каталог.
#
# зависимости: stdlib. фиксирует commit контеста (судят по нему) → воспроизводимо.

import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

HUNT_DIR = os.path.expanduser("~/diplom/hunt")


def get(url, tries=3, timeout=30, raw=False):
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "scsa-hunt/0.1"})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                data = r.read()
                return data if raw else json.loads(data)
        except urllib.error.HTTPError as e:
            # 404/403 — definitive (приватный/нет файла): НЕ ретраим, экономим время
            if e.code in (403, 404, 451):
                return None
            sys.stderr.write(f"  retry {i+1}/{tries} {url}: HTTP {e.code}\n")
            time.sleep(2)
        except Exception as e:  # noqa: BLE001
            sys.stderr.write(f"  retry {i+1}/{tries} {url}: {e}\n")
            time.sleep(2)
    return None


def slug(s):
    return re.sub(r"[^a-zA-Z0-9]+", "_", (s or "contest")).strip("_")


def pick_target(arg):
    with open(os.path.join(HUNT_DIR, "targets.json")) as f:
        targets = json.load(f)
    if arg.isdigit():
        idx = int(arg) - 1
        if 0 <= idx < len(targets):
            return targets[idx]
    for t in targets:
        if arg.lower() in (t.get("title") or "").lower():
            return t
    return None


def resolve_tree(repo, commit):
    """список путей в репо на данном commit (recursive tree). None если репо/commit не достать."""
    ref = commit or "main"
    tree = get(f"https://api.github.com/repos/{repo}/git/trees/{ref}?recursive=1")
    if not tree or "tree" not in tree:
        # fallback: main/master
        for br in ("main", "master"):
            tree = get(f"https://api.github.com/repos/{repo}/git/trees/{br}?recursive=1")
            if tree and "tree" in tree:
                return tree, br
        return None, None
    return tree, ref


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: fetch_scope.py <#|title>")
    t = pick_target(sys.argv[1])
    if not t:
        sys.exit("цель не найдена в hunt/targets.json (сначала find_targets.py)")

    title = t.get("title") or "contest"
    out_dir = os.path.join(HUNT_DIR, "repos", slug(title))
    os.makedirs(out_dir, exist_ok=True)

    # кандидаты репо: СНАЧАЛА публичное sherlock-audit зеркало (scope-репо протокола часто
    # приватные — sherlock-scoping/…), потом сами scope-репо по commit (вдруг публичны)
    all_scope_files = set()
    for s in (t.get("scope_repos") or []):
        all_scope_files |= set(s.get("files") or [])
    candidates = []
    if t.get("audit_repo"):
        candidates.append((t["audit_repo"], "", all_scope_files))
    for s in (t.get("scope_repos") or []):
        if s.get("repo"):
            candidates.append((s["repo"], s.get("commit") or "", set(s.get("files") or [])))

    if not candidates:
        sys.exit(f"у цели '{title}' нет репо в targets.json (площадка {t['platform']} — gated?)")

    total_ok = 0
    for repo, commit, scope_files in candidates:
        sys.stderr.write(f"репо {repo} @ {commit or 'main'}\n")
        tree, ref = resolve_tree(repo, commit)
        if not tree:
            sys.stderr.write(f"  дерево недоступно (приватный репо?)\n")
            continue
        paths = [e["path"] for e in tree["tree"] if e.get("type") == "blob"
                 and e["path"].endswith(".sol")]
        # in-scope по СУФФИКСУ пути (audit-зеркало префиксует репо: tare-io__…/contracts/X.sol
        # против scope-пути contracts/X.sol) — точное совпадение не сработает
        def in_scope(p):
            return any(p == sf or p.endswith("/" + sf) for sf in scope_files)
        matched = set(p for p in paths if in_scope(p)) if scope_files else set()
        want = set(matched)
        # + всё под .../contracts|src|lib для компилируемости импортов (не только in-scope)
        want |= set(p for p in paths if re.search(r"(^|/)(src|contracts|lib|packages)/", p))
        if not want:
            want = set(paths)  # плоский репо — берём все .sol
        want = sorted(want)[:400]  # предохранитель от гигантских репо
        sys.stderr.write(f"  .sol в репо: {len(paths)}, качаем: {len(want)}"
                         f" (in-scope помечены отдельно)\n")
        for p in want:
            dst = os.path.join(out_dir, p)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.exists(dst) and os.path.getsize(dst) > 0:
                total_ok += 1
                continue
            data = get(f"https://raw.githubusercontent.com/{repo}/{ref}/{p}", raw=True)
            if data:
                with open(dst, "wb") as f:
                    f.write(data)
                total_ok += 1
            else:
                sys.stderr.write(f"    MISS {p}\n")
        # сохраним РЕАЛЬНЫЕ in-scope пути (как в дереве) рядом — для приоритезации триажа
        if matched:
            with open(os.path.join(out_dir, "_scope.txt"), "w") as f:
                f.write("\n".join(sorted(matched)) + "\n")
        if total_ok:
            break  # первый успешный репо

    print(f"скачано {total_ok} .sol → {out_dir}")
    print(f"дальше: ./hunt_batch.sh --dir {out_dir}")
    if os.path.exists(os.path.join(out_dir, "_scope.txt")):
        print(f"in-scope пути: {out_dir}/_scope.txt (сверяйся при триаже)")


if __name__ == "__main__":
    main()
