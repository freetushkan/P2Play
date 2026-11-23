# P2Play / MediaMesh — рабочий план

Обновлено: 2026-11-24. Это компактный сопровождаемый план для ветки разработки.
Исходный большой документ `P2Play_MediaMesh.docx` остаётся в ветке `docs`; этот файл
фиксирует состояние кода и ближайшие проверяемые шаги. Статусы ниже относятся к исходникам
и тестам репозитория, а не к проверкам на всех целевых устройствах.

## Готово

### Этапы 0–2: ядро, стример и API

- [x] Общая C++-библиотека ядра и автономный демон; Linux, Android, Switch, PS4 и PS5
      имеют отдельные сборочные обвязки.
- [x] TorrServer MatriX API, API по схеме Silo, авторизация, пользователи и прогресс.
- [x] Linux CI собирает приложение и запускает API-, стриминговые и сетевые тесты.

### Этап 3: списки и сеть MediaMesh

- [x] Ed25519-ключ списка, канонический подписанный bencode-манифест, append-only
      сегменты, проверка подписи, sequence и хэшей.
- [x] BEP 44 указатель `mediamesh.v1`, DHT-публикация/поиск, сохранение указателей и
      защита от rollback/equivocation.
- [x] Рой каталога на фиксированном торренте; BEP 10 `have` и `get`; узел каталога
      может работать без стримера.
- [x] BEP 10 `item`: ответ на `get` и ограниченный push при новой версии. Push идёт
      после `have` с новой sequence; неподтверждённые unsolicited items отбрасываются.
- [x] Приём указателя через общий worker, проверка подписи до загрузки, manifest-first
      приём версии, bootstrap через peer hints и работа с отключённым DHT.
- [x] Сегменты-шарды, immutable media packs, выбор шардов, лимиты хранения, ленивое
      получение паков, snapshots/compaction и проверки целостности.
- [x] PublisherProxy на уровне libtorrent-сессии и документированный SOCKS5/UDP
      ASSOCIATE / VPN deployment; автоматическая Linux-проверка OS kill switch
      выполняется отдельно в disposable network namespace с nftables.
- [x] Интеграционные тесты `mesh-test.py`, `meshnet-test.py`, `peer-items-test.py`,
      `mesh-media-test.py`, `mesh-selective-test.py` и `proxy-test.py`.

## Осталось / следующий порядок

### Этап 3: завершение обмена и эксплуатации

- [x] Проверить push `item` на нескольких узлах и восстановление пропущенного push через
      `get` после перезапуска подписчика; сценарий проходит Linux CI.
- [x] Закрепить в CI лимиты очереди: 128 глобально, 8 на TCP source IP, 32 для DHT;
      unit-тест проверяет квоты между соединениями, дедупликацию, резерв DHT и round-robin.
- [x] Подготовлен implementation-profile draft wire-format и формата списка v1:
      `docs/mediamesh-v1.md`. Документ описывает фактическую реализацию P2Play.
- [x] Проверить wire-совместимость BEP 10 с независимым reference peer в Linux CI:
      handshake, канонические `have`/`get`, обмен `item` в обе стороны и relay.
      Граница проверки (без собственной реализации BEP 44/DHT/v2 transfer) описана
      в `docs/mediamesh-v1.md`.
- [x] Реализован Silo plugin: поиск/обнаружение/подписка, server-side daemon adapter,
      защищённые настройки и native add contract; тесты проверяют закреплённый Silo
      runtime, auth/CSRF, ZIP loader и API.
- [x] Проверен полный live-путь Silo ↔ P2Play с реальным daemon и native torrent engine:
      поиск, подписка, получение каталога, добавление торрента и передача медиаданных
      через authenticated stream API. Linux CI собирает закреплённый Silo, запускает
      три узла MediaMesh, локальные tracker/seeder и проверяет реальные payload bytes.
      Граница: это серверный live acceptance test, не браузерное декодирование видео.
- [x] Прогнать PublisherProxy через настоящий UDP-capable SOCKS5 (Dante): TCP CONNECT,
      UDP ASSOCIATE, DHT/peer traffic и аудит системных вызовов без прямого обхода;
      сценарий проходит Linux CI.
- [x] Проверен OS kill switch в Linux network namespace с veth и default route:
      nftables разрешает только TCP к Dante SOCKS endpoint и UDP к ограниченному
      relay range; прямые TCP/UDP probes блокируются и учитываются drop counter.
      P2Play выполняет DHT и BitTorrent peer traffic через Dante; после остановки
      Dante проверяется отсутствие прямого fallback при работающем P2Play и
      сохранённом firewall. Сценарий проходит Linux CI (tools/proxy-netns-test.py).
      Граница: это изолированный CI acceptance test, не проверка production firewall
      или реального WireGuard-провайдера.
- [x] Проверить стартовые лимиты сегментов, описаний и постеров на реальном большом
      списке и получить первый измеряемый baseline. Linux CI профиль
      `tools/mesh-scale-test.py` проходит на двух реальных узлах: 20 000 выбранных
      записей, 512 записей в невыбранном shard, граничные description/poster объекты,
      lazy fetch и cold restart. Run #103: публикация 1,141 с, подписка 6,776 с, поиск
      0,0244 с, cold start 0,402 с; выбранные сегменты занимают 414 270 B на подписчике,
      два запрошенных media pack — 2 162 688 B. Полный baseline, loopback, disk и RSS
      измерения записаны в `docs/media-shards.md`. Это наблюдение одного runner, не
      жёсткий performance budget.

### Этап 4: клиент и мета-списки

- [x] Зафиксировать источник порядка работ по мета-спискам: исходный DOCX в ветке
      `docs` — общий справочный документ, его табличная позиция F12 не переопределяет
      последовательность реализации. Технический порядок задаёт этот файл в `master`:
      мета-списки относятся к этапу 4 и начинаются после завершения проверок этапа 3.
- [x] Мета-списки: ссылки на другие публичные ключи, подписанные аннотации и флаги
      enabled, автоматическая подписка только на включённые обычные каталоги, отключение
      отдельных элементов и защита от циклов/вложенности (глубина не более 1). Реализованы
      в PR #10: manifest v1, metadata-only release, API, Silo display и проверки подписи,
      смены selection, отказа от meta→meta и восстановления после перезапуска. Linux,
      Switch, Android, PS4, PS5, PS5 package и отдельный Silo plugin CI jobs прошли.
      Спецификация: `docs/meta-lists.md` и `docs/mediamesh-v1.md`.
- [x] Отдельный публичный endpoint идентичности и версии P2Play:
      `GET /api/p2play/version` возвращает `{"name":"P2Play","version":"0.1"}` без
      MatriX/TorrServer/Silo compatibility prefixes. Версия берётся из единственного
      CMake VERSION_MAJOR/MINOR источника. Endpoint работает без токена; legacy
      `/api/system/ping`, `/api/system/version` и MatriX `/echo` не изменены.
      Реализован в PR #12; CI и проверки совместимости прошли.
- [x] Базовая внешняя интеграция pPlay реализована в `freetushkan/pPlay/dev`:
      отдельные `src/net/p2play.h/.cpp`, проверка native version, Bearer login,
      Discover/My lists, запрос подписки, enabled meta-list entries, просмотр по
      тегам, добавление записи в библиотеку и воспроизведение через native stream API.
      Пользователь подтвердил сборку; P2Play-код не использует реализацию `torrserve.*`.
- [ ] Завершить внешний P2Play-клиент: свободный текстовый поиск, карточки записи с
      постером/описанием, продолжение просмотра и синхронизация прогресса.
- [ ] Встроенный режим pPlay: отдельный тип сети `P2Play_embedded` рядом с
      внешним `P2Play`. Встраивать только static `p2play_core` из отдельного
      `git clone` репозитория P2Play в workflow pPlay (с pin конкретного commit);
      standalone daemon не включать в приложение. Внешняя сеть `P2Play` остаётся
      самостоятельным клиентом и не меняет поведение.
      Встроенный core запускается лениво только при выборе `P2Play_embedded`, а при
      переходе в другой модуль останавливается после завершения активных HTTP/stream
      операций. Данные сервера находятся в отдельном каталоге данных pPlay.
      HTTP API должен быть доступен другим устройствам той же локальной сети: bind
      только на `127.0.0.1` запрещён. Не настраивать автоматический проброс портов
      во внешний Интернет; сетевой доступ должен контролироваться обычной LAN-сетью
      и firewall устройства.
      Username/password из настроек сети `P2Play_embedded` — реальные учётные данные
      владельца встроенного сервера. Логин владельца задаётся пользователем и не
      обязан называться `owner`; внутренний owner ID может оставаться стабильным.
      При первом запуске эти данные создают постоянную учётную запись owner, при
      последующих запусках существующие данные авторизации нельзя молча перезаписывать.
      Встроенный режим должен предоставлять создание дополнительных пользователей
      сервера через интерфейс pPlay и существующий защищённый API.
- [ ] Роли и разрешения пользователей: текущий backend использует числовой
      иерархический `rank`, но это ещё не модель именованных ролей и матрица
      возможностей. На отдельном этапе определить роли, разрешения, назначение и
      проверку доступа к действиям, не подменяя их неявными сравнениями чисел.
- [ ] Целевые сборки pPlay со встроенным P2Play проходят для Linux, Android, Switch,
      PS4 и PS5; проверены LAN-доступ со второго устройства, запуск/остановка,
      сохранность owner и дополнительных пользователей, а также отсутствие listener
      после деактивации.

### Этап 5: наполнение каталога

- [ ] Граббер как отдельный инструмент: адаптеры трекеров и архивных дампов,
      нормализация, теги, постеры/описания и выгрузка NDJSON.
- [ ] Репрезентативный большой набор данных и измерения размера сегментов, индекса,
      паков и времени поиска; на их основе уточнить лимиты клиентов.

## Инварианты протокола

- Идентификатор списка — публичный ключ, не имя или домен.
- `have` — только подсказка о версии; доверие даёт подписанный BEP 44 `item`.
- `item` в push принимается только при совпадении ключа и sequence с последним
  `have` отправителя; ответ на `get` принимается только для активного запроса.
- Сеть и DHT не должны блокировать BEP 10 callback: проверка, файловый ввод-вывод и
  добавление торрента выполняются в ограниченной очереди рабочего потока.
- До проверки подписанного манифеста скачивается только манифест; файлы/приоритеты
  определяются только подписанными дескрипторами и локальной политикой.
- DHT — один из путей доставки, а не источник доверия; peer hints и relay не заменяют
  криптографическую проверку.

## Команды проверки

```sh
python3 -I tools/peer-items-test.py build-linux/src/app/p2playd
python3 -I tools/mediamesh-interop-test.py build-linux/src/app/p2playd
python3 -I tools/mesh-test.py build-linux/src/app/p2playd
python3 -I tools/mesh-selective-test.py build-linux/src/app/p2playd
python3 -I tools/mesh-media-test.py build-linux/src/app/p2playd
python3 -I tools/proxy-test.py build-linux/src/app/p2playd --trace
python3 -I tools/proxy-dante-test.py build-linux/src/app/p2playd --trace
python3 -I tools/proxy-netns-test.py build-linux/src/app/p2playd
python3 -I tools/ui-test.py build-linux/src/app/p2playd
python3 tools/silo-live-test.py build-linux/src/app/p2playd
```

`meshnet-test.py` additionally uses the Python `libtorrent` binding; protocol tests that
exercise the BEP 10 pointer path use only the standard library.
