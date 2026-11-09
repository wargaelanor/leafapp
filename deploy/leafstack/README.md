# LeafStack — сервер LeafControl одним docker-compose.yml

Серверный стек для телеметрии электромобиля: карта и поездки, база измерений,
дашборды, опционально удалённое управление. Разворачивается на любом Linux-хосте
с Docker (NAS, мини-ПК, VPS, Raspberry Pi).

**Всё в одном файле.** `docker-compose.yml` не требует соседних каталогов:
контейнер `grafana-provision` сам создаёт datasource InfluxDB и дашборд
LeafControl.

| Сервис | Порт | Для чего |
|---|---:|---|
| **Traccar** | 8925 | карта, поездки/стоянки, отчёты, приём координат |
| **Grafana** | 3000 | дашборд: SOC, V/A, 12 В, температуры, кВт·ч |
| **InfluxDB 2.7** | 8086 | хранилище телеметрии (bucket `telemetry`, org `leaf`) |
| **Portainer** | 9443 | управление контейнерами (профиль `admin`) |
| **Tailscale** | — | доступ из интернета без проброса портов (профиль `remote`) |

---

## 1. Запуск

Нужен Docker с compose v2 (`docker compose version`).

```bash
mkdir leafstack && cd leafstack
# положите сюда docker-compose.yml
cp .env.example .env      # заполнить INFLUX_PASSWORD, INFLUX_TOKEN, GRAFANA_PASSWORD
docker compose up -d
docker compose ps
```

Проверка:

```bash
curl -s http://localhost:8086/health                                  # {"status":"pass"}
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:3000/login  # 200
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8925/       # 200
```

Логин Grafana — `GRAFANA_USER` / `GRAFANA_PASSWORD` из `.env`.
Логин InfluxDB — `INFLUX_ADMIN_USER` / `INFLUX_PASSWORD` из `.env`.
Дашборд «LeafControl — телеметрия» лежит в папке `Leaf` и уже подключён
к InfluxDB.

Обновление и остановка:

```bash
docker compose pull && docker compose up -d
docker compose down            # данные остаются в томах
```

Данные: тома `leafstack_influx-data`, `leafstack_grafana-data`,
`leafstack_traccar-data`, `leafstack_grafana-prov`.

---

## 2. Удалённый доступ

### Вариант A — Tailscale (рекомендуется)

Без белого IP, за CG-NAT, без проброса портов; наружу ничего не публикуется.

1. Ключ: https://login.tailscale.com/admin/settings/keys (Reusable) → в `.env`
   `TS_AUTHKEY=tskey-auth-...`
2. `docker compose --profile remote up -d`
3. На телефоне/ноутбуке войти в тот же tailnet. Стек откроется по имени:

```
http://leafstack:3000     Grafana
http://leafstack:8925     Traccar
http://leafstack:8086     InfluxDB
https://leafstack:9443    Portainer (после --profile admin)
```

Если на хосте нет `/dev/net/tun` (часть NAS): `TS_USERSPACE=true` и
`docker exec leafstack-tailscale tailscale serve --bg 3000`.

### Вариант B — домены и проброс на роутере

| Имя | Снаружи | Куда |
|---|---|---|
| `grafana.example.com` | 443 HTTPS | `http://HOST:3000` |
| `traccar.example.com` | 443 HTTPS | `http://HOST:8925` |
| `portainer.example.com` | 443 HTTPS | `https://HOST:9443` |

Grafana и Traccar внутри — **HTTP**, Portainer — **HTTPS**. Неверный протокол
даёт `502 Bad Gateway` или `Client sent an HTTP request to an HTTPS server`.

InfluxDB (`8086`) наружу обычно не нужен: Grafana ходит в неё по внутренней
сети Docker.

---

## 3. Подключение платы

**Traccar (позиции):**

```text
GET https://traccar.example.com/api/positions
    ?id=Leaf&lat=55.75&lon=37.62&speed=42&bearing=180&timestamp=<unix>
&apiKey=<ключ устройства>
```

Ключ: Traccar → *Settings → Devices → `Leaf` → API key*.

**InfluxDB (телеметрия), line protocol, precision=s:**

```text
leaf    soc=86.5,pack_v=351.2,pack_i=-8.1,pack_kw=-2.8,v12=12.6,i12=-0.4,
        amb=3.5,cabin=8.0,batt=11.2,odo=149881,gear=1i,spd=0i,
        charging=1i,plugged=1i  <ts_s>

trip    kwh=1.58,km=5.0,dur_s=508,soc0=86,soc1=82,cost=11.85   <ts_end>

charge  kwh=2.45,dur_s=6262,soc0=88,soc1=97,cost=9.80          <ts_end>
```

Отправка: `POST /api/v2/write?org=leaf&bucket=telemetry&precision=s`,
заголовок `Authorization: Token <INFLUX_TOKEN>`, тег `device=Leaf`.
Имена measurement/field в дашборде рассчитаны именно на этот контракт.

---

## 4. Если что-то не открывается

**`502 Bad Gateway`.** Сервис внутри не слушает порт — смотрите
`docker compose logs -f grafana` (или `traccar`).

**Grafana упала после переезда на каталоги хоста.** При bind-mount вместо тома
каталог должен быть доступен пользователю контейнера. Простейшее решение —
добавить сервису `user: "0:0"`.

**Порт занят.** Поменяйте `*_PORT` в `.env`.

**Стек не стартует из-за переменных.** Это защита: заполните `INFLUX_PASSWORD`,
`INFLUX_TOKEN`, `GRAFANA_PASSWORD`.

**Нет данных в Grafana.** Проверьте, что плата пишет в InfluxDB:

```bash
curl -s -H "Authorization: Token $INFLUX_TOKEN" \
  "http://localhost:8086/api/v2/query?org=leaf" \
  -H "Content-Type: application/vnd.flux" \
  --data 'from(bucket:"telemetry")|>range(start:-1h)|>count()'
```

**Дашборд в UI только для чтения.** Он создан provisioning-контейнером. Чтобы
править в интерфейсе — скопируйте его (Save as) или правьте JSON в
`docker-compose.yml` (секция `grafana-provision`) и перезапустите стек.

---

## 5. Перенос на другой хост

Нужны: `docker-compose.yml`, `.env` (свои секреты) и, при необходимости,
данные из томов. Всё остальное создаётся автоматически.
