# LeafStack — Traccar + InfluxDB + Grafana + Portainer для LeafControl

Один docker-compose: сервер телеметрии для платы и приложения, с веб-интерфейсами
и удалённым управлением. Рассчитан на TrueNAS Scale (красивые CGI-приложения не
нужны — Custom App с этим YAML) или любой хост с docker compose.

## Состав и порты (на хосте)

| Сервис | Порт хоста | Что внутри | Доступ |
|---|---|---|---|
| Traccar | **8925** (web/API), 5055 (tcp) | трекинг, поездки, стоянки, отчёты, Android Traccar Manager | браузер + API |
| InfluxDB 2.7 | **8086** | хранение телеметрии (bucket `telemetry`, org `leaf`) | API + свой UI |
| Grafana | **3000** | готовые дашборды Leaf (SOC, V/A, 12В, температуры, кВт·ч, одометр) | браузер |
| Portainer | **9443** | удалённое управление контейнерами (создать/поправить/перезапустить) | браузер |

## Запуск (обычный docker-хост)

```bash
cd leafstack
cp .env.example .env      # заполнить секреты (openssl rand -hex 16)
# тома лежат в ./data - заменить на датасет NAS, если надо
docker compose up -d
docker compose ps
```

## Запуск на TrueNAS Scale

Apps → Discover → **Custom App** → Install:
1. Name: `leafstack`
2. Включить "Use your own YAML" и вставить содержимое `docker-compose.yml`
3. В YAML заменить `./data/...` на путь датасета, например
   `/mnt/storage/apps/leafstack/data/...` (создать датасет заранее)
4. Переменные окружения (INFLUX_PASSWORD, INFLUX_TOKEN, GRAFANA_PASSWORD...)
   добавить в секцию `environment` каждого сервиса (TrueNAS редактирует YAML
   целиком — значения берутся из `.env`)
5. Порты 8925/3000/8086/9443 не должны конфликтовать (у TrueNAS own 80/443 —
   не пересекаются)

## KeenDNS (удалённый доступ)

В админке Keenetic создать правила (через облако KeenDNS или проброс портов,
как уже сделано для `tru`), каждый на IP машины с Docker'ом:

- `leaf.<...>.keenetic.link`   → :8925  (Traccar, HTTPS)
- `grafana.<...>.keenetic.link`→ :3000  (Grafana)
- `portainer.<...>.keenetic.link`→:9443 (Portainer — управление стеком извне)
- `influx.<...>.keenetic.link` → :8086  (опционально: чтение из PWA;
  если PWA ходит через Grafana/TRaccar API — не нужен)

## Первый запуск

1. **Traccar** — открыть `https://leaf...:8925`/ — мастер создаст администратора.
   Создать устройство `Leaf` → Settings → Device → API key. Плата будет слать
   позиции на `GET /api/positions?id=Leaf&apikey=<ключ>&lat=..&lon=..&...`
2. **InfluxDB** — поднимется уже инициализированным (org=leaf, bucket=telemetry,
   токен из .env). Проверка: `curl -H "Authorization: Token $INFLUX_TOKEN"
   "http://host:8086/api/v2/query?org=leaf" -d '{"query":"from(bucket:\"telemetry\")|>range(start:-1h)"}' -H "Content-Type: application/json"`
3. **Grafana** — `http://host:3000`, логин из .env; дашборд «LeafControl —
   телеметрия» уже в папке Leaf, дата-сорс подключён.
4. **Portainer** — `https://host:9443`, первый заход создаёт админа; далее
   можно обновлять образы и перезапускать стек прямо из веб-UI.

## Контракт данных (пишет прошивка, читает Grafana/приложение)

Line protocol, bucket `telemetry`, tag `device=Leaf`:

```
leaf    soc=86.5,pack_v=351.2,pack_i=-8.1,pack_kw=-2.8,v12=12.6,i12=-0.4,
        amb=3.5,cabin=8.0,batt=11.2,odo=149881,gear=1i,spd=0i,
        charging=1i,plugged=1i  <ts_s>

trip    kwh=1.58,kwh_day=1.58,kwh_night=0,km=5.0,dur_s=508,soc0=86,soc1=82,
        cost=11.85  <ts_end>          # одна запись на завершение поездки

charge  kwh=2.45,kwh_day=0,kwh_night=2.45,dur_s=6262,soc0=88,soc1=97,
        cost=9.80, sessions=1i        # одна запись на завершение зарядки
```

Частота: `leaf` — раз в 5 мин при активности шины + сразу при старте/финише
сессии; `trip`/`charge` — по одному событию на завершение. Невысланные точки
копятся в кольцевом буфере NVS (недели данных) и доезжают после пропажи сети —
поездки больше не теряются.

## Что делает прошивка дальше (готовится, .137)

- модуль `leafPush`: HTTPS POST в Influx (`/api/v2/write`) + позиция Traccar
  (`/api/positions`, lat/lon из GPS модема A7670 на T-2CAN)
- накопленный лог поездок/зарядок выгружается историей (разовый бэкап-флеш)
- настройки в /settings: `server_base`, `influx_token`, `traccar_key`, вкл/выкл
