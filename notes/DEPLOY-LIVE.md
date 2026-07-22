# Витрина SCSA — ЖИВОЙ деплой

## Доступ
- **URL:** https://scsa.95-165-81-234.sslip.io
- **Логин:** `admin`  ·  **Пароль:** `MYXKtwgRsE7`  *(демо-пароль, basic-auth; хэш в `~/scsa/deploy/.basicauth` на homeserver)*
- Работает: скан по `.sol` и по адресу (Etherscan-фетч). Гейт — basic-auth в Caddy.

## Где крутится
- Машина: **homeserver** (`192.168.1.8`, публичный IP `95.165.81.234`), Ubuntu 24.04.
- Код: `~/scsa` (rsync из `~/diplom` на рабочей машине).
- Контейнер: `deploy-backend-1`, слушает `127.0.0.1:8090` (наружу — только через Caddy).
- Compose: `~/scsa/deploy/docker-compose.homeserver.yml` (только backend; свой Caddy НЕ поднимали).
- Публичный вход: **существующий host-Caddy** (`/etc/caddy/Caddyfile`), отдельный site-блок в конце файла.
  Бэкапы Caddyfile: `/etc/caddy/Caddyfile.bak-*`.
- solc крутится на демоне хоста через проброшенный `/var/run/docker.sock`.

## Управление (по SSH: `ssh akum1@192.168.1.8`)
```bash
cd ~/scsa/deploy
docker compose -f docker-compose.homeserver.yml logs -f backend     # логи
docker compose -f docker-compose.homeserver.yml restart backend     # рестарт
docker compose -f docker-compose.homeserver.yml up -d --build        # обновить (после rsync нового кода)
```
Обновить код с рабочей машины:
```bash
rsync -az --exclude solidity-llvm/build --exclude web/node_modules --exclude web/dist \
  -e "ssh -i ~/.ssh/id_ed25519" ~/diplom/ akum1@192.168.1.8:~/scsa/
```

## Чистый домен вместо sslip.io (когда захочешь)
sslip.io — временный публичный wildcard (URL некрасивый, зависит от их сервиса). Для постоянного:
1. У DNS-провайдера `amberzamotin.ru` добавить запись: **`scsa` A `95.165.81.234`**.
2. В `/etc/caddy/Caddyfile` заменить `scsa.95-165-81-234.sslip.io {` → `scsa.amberzamotin.ru {`.
3. `sudo caddy validate --adapter caddyfile --config /etc/caddy/Caddyfile && sudo systemctl reload caddy`.
Caddy сам выпустит cert для `scsa.amberzamotin.ru`.

## Автозапуск/надёжность
- Контейнер: `restart: unless-stopped` → переживает ребут (если docker-сервис в автозапуске).
- Caddy: systemd-сервис, cert-ы авто-продление.
- ⚠️ docker.sock проброшен в контейнер (root-эквивалент на хосте) — доступ закрыт basic-auth’ом, публично не светить без пароля.
