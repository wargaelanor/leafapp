# LeafStack — сервер LeafControl в одном docker compose

Готовый серверный стек для телеметрии электромобиля: карта и поездки, база
измерений, дашборды и удалённое управление. Разворачивается одной командой на
любом Linux-хосте с Docker (NAS, мини-ПК, VPS, Raspberry Pi 4/5).

| Сервис | Порт | Для чего |
|---|---:|---|
| **Traccar** | 8925 | карта, поездки/стоянки, отчёты, приём координат от платы |
| **Grafana** | 3000 | дашборд: SOC, напряжение/ток, 12 В, температуры, кВт·ч |
| **InfluxDB 2.7** | 8086 | хранилище телеметрии (bucket `telemetry`, org `leaf`) |
| **Portainer** | 9443 | управление контейнерами из браузера (профиль `admin`) |
| **Tailscale** | — | доступ из интернета без проброса портов (профиль `remote`) |

Grafana уже настроена: дата-сорс на InfluxDB и дашборд «LeafControl —
телеметрия» подключаются автоматически, руками ничего создавать не нужно.

---

## 1. Запуск (5 минут)

Нужен Docker с плагином compose v2 (`docker compose version`).

```bash
cd leafstack
cp .env.example .env
# отредактируйте .env: INFLUX_PASSWORD, INFLUX_TOKEN, GRAFANA_PASSWORD
docker compose up -d
docker compose ps
```

Проверка:

```bash
curl -s http://localhost:8086/health          # {"status":"pass"}
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:3000/login   # 200
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8925/        # 200
```

Логин в Grafana — `GRAFANA_USER` / `GRAFANA_PASSWORD` из `.env`.
Логин в InfluxDB — `INFLUX_ADMIN_USER` / `INFLUX_PASSWORD` из `.env`.

Остановить / обновить:

```bash
docker compose down          # остановить (данные сохраняются в томах)
docker compose pull && docker compose up -d   # обновить образы
```

Данные лежат в именованных томах `leafstack_influx-data`,
`leafstack_grafana-data`, `leafstack_traccar-data` — они переживают
перезапуск и обновление контейнеров.

---

## 2. Удалённый доступ

### Вариант A — Tailscale (рекомендуется)

Работает без белого IP, за CG-NAT, без проброса портов. Трафик внутри
зашифрованного tailnet, наружу ничего не публикуется.

1. Создайте ключ: https://login.tailscale.com/admin/settings/keys (Reusable).
2. Впишите его в `.env`: `TS_AUTHKEY=tskey-auth-...`
3. Запустите:

```bash
docker compose --profile remote up -d
```

4. На телефоне/ноутбуке поставьте Tailscale и войдите в тот же аккаунт.
   Стек будет доступен по имени из `TS_HOSTNAME`:

```
http://leafstack:3000     Grafana
http://leafstack:8925     Traccar
http://leafstack:8086     InfluxDB
https://leafstack:9443    Portainer
```

Устройство появится в админке Tailscale; доступ можно ограничить ACL.

Если на хосте нет `/dev/net/tun` (часть NAS), поставьте `TS_USERSPACE=true`
и опубликуйте сервисы через serve внутри контейнера:

```bash
docker exec leafstack-tailscale tailscale serve --bg 3000
```

### Вариант B — проброс портов / reverse proxy на роутере

Если есть белый IP, на роутере (Keenetic, MikroTik, OpenWrt) сделайте правила
на адрес хоста со стеком:

| Имя | Порт снаружи | Куда |
|---|---|---|
| `grafana.example.com` | 443 (HTTPS) | `http://HOST:3000` |
| `traccar.example.com` | 443 (HTTPS) | `http://HOST:8925` |
| `portainer.example.com` | 443 (HTTPS) | `https://HOST:9443` |

Важно: Grafana и Traccar внутри работают по **HTTP**, Portainer — по
**HTTPS**. Если в правиле указать не тот протокол, получите `502 Bad Gateway`
или `Client sent an HTTP request to an HTTPS server`.

InfluxDB (`8086`) наружу публиковать не нужно: Grafana ходит в неё по
внутренней сети docker. Открывайте только если нужен внешний клиент.

---

## 3. Подключение платы LeafControl

Плата шлёт данные в двух форматах:

**Traccar (позиции):**

```text
GET https://traccar.example.com/api/positions
    ?id=Leaf&lat=55.75&lon=37.62&speed=42&bearing=180&timestamp=<unix>
    &deviceId=Leaf&apiKey=<ключ устройства>
```

Ключ создаётся в Traccar: *Settings → Devices → добавить `Leaf` → API key*.

**InfluxDB (телеметрия), line protocol:**

```text
leaf    soc=86.5,pack_v=351.2,pack_i=-8.1,pack_kw=-2.8,v12=12.6,i12=-0.4,
        amb=3.5,cabin=8.0,batt=11.2,odo=149881,gear=1i,spd=0i,
        charging=1i,plugged=1i  <ts_s>

trip    kwh=1.58,km=5.0,dur_s=508,soc0=86,soc1=82,cost=11.85   <ts_end>

charge  kwh=2.45,dur_s=6262,soc0=88,soc1=97,cost=9.80          <ts_end>
```

Отправка: `POST /api/v2/write?org=leaf&bucket=telemetry&precision=s`
с заголовком `Authorization: Token <INFLUX_TOKEN>` и тегом `device=Leaf`.

Поля дашборда ожидают именно эти имена measurement/field — если плата пишет
другие, поправьте запросы на панелях или приведите имена к контракту.

---

## 4. Если что-то не открывается

**`502 Bad Gateway` на Grafana/Traccar.** Сервис внутри не слушает порт.
Смотрите логи: `docker compose logs -f grafana`. Частая причина — права на
каталог данных: если вместо именованного тома подключили каталог хоста
(`- ./data/grafana:/var/lib/grafana`), каталог должен быть доступен на запись
пользователю контейнера. Простейшее решение — добавить сервису `user: "0:0"`.

**Порт занят.** `docker compose up` скажет `port is already allocated` —
поменяйте `*_PORT` в `.env`.

**Не стартует из-за пустых переменных.** Это защита: заполните
`INFLUX_PASSWORD`, `INFLUX_TOKEN`, `GRAFANA_PASSWORD` в `.env`.

**Нет данных в Grafana.** Проверьте, что плата реально пишет в InfluxDB:

```bash
curl -s -H "Authorization: Token $INFLUX_TOKEN" \
  "http://localhost:8086/api/v2/query?org=leaf" \
  -H "Content-Type: application/vnd.flux" \
  --data 'from(bucket:"telemetry")|>range(start:-1h)|>count()'
```

---

## 5. Состав каталога

```
leafstack/
├── docker-compose.yml      стек целиком
├── .env.example            шаблон секретов и портов
├── traccar/conf.xml        настройки Traccar (фильтры, reverse proxy)
└── grafana/
    ├── provisioning/datasources/leaf.yaml    дата-сорс InfluxDB
    ├── provisioning/dashboards/leaf.yaml     автозагрузка дашбордов
    └── dashboards/leaf-telemetry.json        дашборд LeafControl
```

Перенос на другой хост: скопируйте каталог целиком, создайте новый `.env`.
Секреты и данные переносятся отдельно (`.env` и тома/каталоги данных).
