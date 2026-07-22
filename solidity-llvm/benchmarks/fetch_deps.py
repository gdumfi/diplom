#!/usr/bin/env python3
# fetch_deps.py — дотягивает git-submodule ЗАВИСИМОСТИ контест-репо (OZ/solmate/safe/…), которых
# нет в дереве как файлов (gitlink-и). Без них solc/sca не скомпилит. Тянет ТРАНЗИТИВНОЕ ЗАМЫКАНИЕ
# импортов (только реально нужные файлы, не весь OpenZeppelin) → быстрее и по throttled-сети.
#
# usage: fetch_deps.py <#|title>   (после fetch_scope.py)
# результат: hunt/repos/<contest>/<subroot>/lib/<submodule>/... + remappings.txt/foundry.toml.
# дальше sca/hunt_batch компилят с резолвом импортов.
#
# как: parse .gitmodules(url) + gitlink SHA (дерево) + remappings.txt → BFS по import-ам .sol,
# резолвя каждый в submodule-путь и фетча raw@sha. stdlib.

import json
import os
import posixpath
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
            if e.code in (403, 404, 451):
                return None
            time.sleep(2)
        except Exception:  # noqa: BLE001
            time.sleep(2)
    return None


def get_text(url):
    d = get(url, raw=True)
    return d.decode("utf-8", "replace") if d else None


def pick_target(arg):
    with open(os.path.join(HUNT_DIR, "targets.json")) as f:
        targets = json.load(f)
    if arg.isdigit() and 0 <= int(arg) - 1 < len(targets):
        return targets[int(arg) - 1]
    for t in targets:
        if arg.lower() in (t.get("title") or "").lower():
            return t
    return None


def slug(s):
    return re.sub(r"[^a-zA-Z0-9]+", "_", (s or "contest")).strip("_")


def parse_remappings(text):
    remaps = []
    for line in (text or "").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        pre, tgt = line.split("=", 1)
        remaps.append((pre.strip(), tgt.strip()))
    remaps.sort(key=lambda r: len(r[0]), reverse=True)  # longest-prefix wins
    return remaps


def parse_gitmodules(text):
    """path → owner/repo (из github url)."""
    out = {}
    path = None
    for line in (text or "").splitlines():
        s = line.strip()
        if s.startswith("path"):
            path = s.split("=", 1)[1].strip()
        elif s.startswith("url") and path:
            url = s.split("=", 1)[1].strip()
            m = re.search(r"github\.com[:/]+([^/]+/[^/.]+)", url)
            if m:
                out[path] = m.group(1)
            path = None
    return out


IMPORT_RE = re.compile(r'import\s+(?:[^"\';]*?\bfrom\s+)?["\']([^"\']+)["\']')


def resolve(imp, from_projrel, remaps):
    """import из файла from_projrel → project-relative путь (относительно subroot)."""
    if imp.startswith("."):
        return posixpath.normpath(posixpath.join(posixpath.dirname(from_projrel), imp))
    for pre, tgt in remaps:
        if imp.startswith(pre):
            return posixpath.normpath(tgt + imp[len(pre):])
    return imp  # как есть (редко)


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: fetch_deps.py <#|title>")
    t = pick_target(sys.argv[1])
    if not t or not t.get("audit_repo"):
        sys.exit("нет audit_repo у цели (сначала find_targets.py + fetch_scope.py)")
    repo = t["audit_repo"]
    out_dir = os.path.join(HUNT_DIR, "repos", slug(t.get("title")))

    # дерево audit-репо: subroot (где foundry.toml) + gitlink SHA субмодулей
    tree = get(f"https://api.github.com/repos/{repo}/git/trees/main?recursive=1")
    if not tree or "tree" not in tree:
        sys.exit(f"дерево {repo} недоступно")
    ref = "main"
    entries = tree["tree"]
    foundry = next((e["path"] for e in entries if e["path"].endswith("foundry.toml")), None)
    subroot = posixpath.dirname(foundry) if foundry else ""
    gitlink_sha = {e["path"]: e.get("sha") for e in entries if e.get("type") == "commit"}

    # конфиги
    gm = get_text(f"https://raw.githubusercontent.com/{repo}/{ref}/.gitmodules") or \
        get_text(f"https://raw.githubusercontent.com/{repo}/{ref}/{subroot}/.gitmodules")
    submods = parse_gitmodules(gm)  # projrel-path(с subroot) → owner/repo
    rm_text = get_text(
        f"https://raw.githubusercontent.com/{repo}/{ref}/{subroot}/remappings.txt")
    remaps = parse_remappings(rm_text)
    # сохраним конфиги локально (нужны sca/solc для резолва)
    for cfg in ("remappings.txt", "foundry.toml"):
        txt = get_text(f"https://raw.githubusercontent.com/{repo}/{ref}/{subroot}/{cfg}")
        if txt:
            with open(os.path.join(out_dir, subroot, cfg), "w") as f:
                f.write(txt)

    # Foundry резолвит root-relative импорты (src=contracts → import "contracts/X.sol") через
    # base-path; наш Driver ключует sources абсолютно и remapping для голого <src>/ не имеет →
    # добавляем явный self-remapping <src>/=<src>/, иначе solc не найдёт internal-импорты.
    ft_path = os.path.join(out_dir, subroot, "foundry.toml")
    rm_path = os.path.join(out_dir, subroot, "remappings.txt")
    src_dir = "src"
    if os.path.exists(ft_path):
        m = re.search(r'(?m)^\s*src\s*=\s*["\']([^"\']+)["\']', open(ft_path).read())
        if m:
            src_dir = m.group(1).strip("/")
    existing = open(rm_path).read() if os.path.exists(rm_path) else ""
    if f"{src_dir}/=" not in existing:
        with open(rm_path, "a") as f:
            if existing and not existing.endswith("\n"):
                f.write("\n")
            f.write(f"{src_dir}/={src_dir}/\n")

    # projrel субмодуля (без subroot) → (owner/repo, sha)
    sub_by_projrel = {}
    for gm_path, orepo in submods.items():
        pr = gm_path[len(subroot) + 1:] if subroot and gm_path.startswith(subroot + "/") else gm_path
        sub_by_projrel[pr] = (orepo, gitlink_sha.get(gm_path, ""))
    # тест-онли субмодули пропускаем (не нужны для компиляции контрактов)
    SKIP = ("forge-std", "createx-forge", "ds-test")

    local_root = os.path.join(out_dir, subroot)

    def projrel_to_local(pr):
        return os.path.join(local_root, pr)

    def fetch_projrel(pr):
        """скачать файл по project-relative пути (внутренний из audit-репо ИЛИ из субмодуля)."""
        dst = projrel_to_local(pr)
        if os.path.exists(dst) and os.path.getsize(dst) > 0:
            return True
        # субмодуль?
        for sub_pr, (orepo, sha) in sub_by_projrel.items():
            if pr == sub_pr or pr.startswith(sub_pr + "/"):
                if any(s in sub_pr for s in SKIP):
                    return False
                in_sub = pr[len(sub_pr) + 1:]  # путь внутри субмодуль-репо
                url = f"https://raw.githubusercontent.com/{orepo}/{sha or 'main'}/{in_sub}"
                data = get(url, raw=True)
                if data:
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    open(dst, "wb").write(data)
                    return True
                return False
        # внутренний файл audit-репо
        url = f"https://raw.githubusercontent.com/{repo}/{ref}/{subroot}/{pr}"
        data = get(url, raw=True)
        if data:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(data)
            return True
        return False

    # BFS транзитивного замыкания: старт — все уже скачанные .sol контрактов
    seen = set()
    queue = []
    for dp, _, files in os.walk(local_root):
        for fn in files:
            if fn.endswith(".sol"):
                pr = os.path.relpath(os.path.join(dp, fn), local_root).replace(os.sep, "/")
                queue.append(pr); seen.add(pr)

    fetched = 0
    processed = 0
    while queue:
        pr = queue.pop()
        processed += 1
        local = projrel_to_local(pr)
        if not os.path.exists(local):
            if fetch_projrel(pr):
                fetched += 1
            else:
                continue
        try:
            text = open(local, encoding="utf-8", errors="replace").read()
        except Exception:  # noqa: BLE001
            continue
        for imp in IMPORT_RE.findall(text):
            dep = resolve(imp, pr, remaps)
            if dep and dep not in seen and dep.endswith(".sol"):
                seen.add(dep)
                queue.append(dep)
        if processed % 25 == 0:
            sys.stderr.write(f"  обработано {processed}, докачано deps {fetched}\n")

    print(f"deps: докачано {fetched} файлов зависимостей → {local_root}/lib")
    print(f"remappings/foundry.toml сохранены. Дальше: ./hunt_batch.sh --dir {out_dir}")


if __name__ == "__main__":
    main()
