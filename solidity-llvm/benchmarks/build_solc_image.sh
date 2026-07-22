#!/usr/bin/env bash
# build_solc_image.sh — собирает ЛОКАЛЬНЫЙ docker-образ ethereum/solc:<версия> из
# официального статичного бинаря (binaries.soliditylang.org), когда Docker Hub его
# не публикует (upstream ethereum/solc остановился ~0.8.30, а solc-релизы идут дальше).
# Тулза зовёт `docker run ethereum/solc:VER --standard-json` — образ с таким тегом
# подхватывается автоматически, БЕЗ правок кода.
#
# использование:  ./build_solc_image.sh 0.8.35 [0.8.34 ...]
set -euo pipefail

LIST="${SOLC_LIST:-/tmp/solc_list.json}"
[ -f "$LIST" ] || curl -fsSL https://binaries.soliditylang.org/linux-amd64/list.json -o "$LIST"

for V in "$@"; do
    if docker image inspect "ethereum/solc:$V" >/dev/null 2>&1; then
        echo "  ethereum/solc:$V — уже есть"
        continue
    fi
    FN=$(python3 -c "import json;print(json.load(open('$LIST'))['releases'].get('$V',''))")
    [ -n "$FN" ] || { echo "  $V — нет в list.json, пропускаю" >&2; continue; }
    TMP=$(mktemp -d)
    curl -fsSL "https://binaries.soliditylang.org/linux-amd64/$FN" -o "$TMP/solc"
    chmod +x "$TMP/solc"
    cat > "$TMP/Dockerfile" <<'DOCKER'
FROM debian:bookworm-slim
COPY solc /usr/bin/solc
ENTRYPOINT ["/usr/bin/solc"]
DOCKER
    docker build -q -t "ethereum/solc:$V" "$TMP" >/dev/null
    rm -rf "$TMP"
    echo "  собран ethereum/solc:$V  ($FN)"
done
