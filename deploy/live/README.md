# LeafControl Server — одна ссылка на всё

Сервер для платы, PWA и Android-приложения. Заменяет MQTT-брокер и OVMS-сервер:
плата сама выходит на сервер по HTTP, телефон тоже говорит только с сервером.

- **Один адрес**: `https://leaf.wargaelanor.keenetic.link` — управление, телеметрия,
  поездки, зарядки, таймеры климата, админка.
- **Плата как клиент**: генерирует свой логин и пароль, регистрируется на сервере,
  админ одобряет устройство. Дальше плата опрашивает очередь команд.
- **Роли**: `admin` (все устройства, заявки, журнал) и владелец (только свои машины).
- **Хранение**: SQLite, файл `leaf.db` в томе. История чистится по `LEAF_KEEP_DAYS`.

## Эндпоинты

### Плата (Bearer `<device_token>`)

| Метод | Что делает |
|---|---|
| `POST /api/device/enroll` | первая регистрация: `{login, password, name, serial, fw}` → `{device_token, status}` |
| `POST /api/device/heartbeat` | `{link, rssi, bus_age_s}` → `{poll_s}` |
| `POST /api/device/state` | снимок телеметрии (SOC, температуры, одометр…), кладётся в историю |
| `POST /api/device/session` | завершённая поездка/зарядка `{kind:"trip"|"charge", …}` |
| `GET /api/device/cmd?wait=N` | long-poll очереди команд (до 25 с) → `{id, name, args}` |
| `POST /api/device/ack` | подтверждение выполнения `{id, ok, result}` |

### Пользователь (cookie-сессия)

`POST /api/auth/login`, `/logout`, `/password`; `GET /api/devices`, `/api/state`,
`/api/hist`, `/api/sessions`, `/api/session`, `/api/sched`; `POST /api/cmd`,
`/api/sched`, `/api/sched/cancel`, `/api/notify/token` (FCM).

### Админ

`GET /api/admin/devices?status=pending|all`, `POST /api/admin/devices/<id>/approve|reject`,
`GET /api/admin/users`, `/api/admin/events`, `/api/admin/commands`.

## Первый запуск

```bash
export LEAF_ADMIN_PASSWORD='сильный-пароль'
docker compose up -d
```

Открыть `http://<host>:8080`, войти как `admin`.

1. Включить плату — она пришлёт `enroll` со своим логином/паролем.
2. В разделе **Админ → Заявки** нажать «Одобрить».
3. Владелец входит логином и паролем, которые выдала плата.

## Команды

`clim_on` (args: `{minutes}`), `clim_off`, `lock`, `unlock`, `lights`, `horn`,
`refresh`, `charge_start`, `charge_stop`.

Доставка: если плата на связи — `pushed: true`, иначе команда ждёт в очереди и
плата забирает её на следующем опросе (`poll_s`, по умолчанию 10 с).

## Что важно про свежесть данных

В API две разные величины, их нельзя путать:

- `bus_age_s` — сколько секунд назад машина последний раз говорила в CAN-шину
  (актуальность показаний);
- `last_seen` / `online` — когда плата последний раз выходила на сервер
  (живость связи, обновляется даже если машина спит).

## Ограничения текущей версии

- FCM: регистрация токенов реализована, отправка push требует
  `google-services.json` и ключа сервера (пока не подключено).
- Прошивка платы: модуль выхода на сервер добавляется отдельно (см. firmware).
