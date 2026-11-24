# P2Play / MediaMesh — рабочий план

Обновлено: 2026-11-24. Это компактный сопровождаемый план для ветки разработки.
Платформенная матрица и границы Android/Silo описаны отдельно в
`docs/platform-matrix.md`; исходный DOCX в ветке `docs` остаётся справочным.
Исходный большой документ `P2Play_MediaMesh.docx` и его текстовая версия `P2Play_MediaMesh.md`
сохранены в ветке `docs`; этот файл фиксирует состояние кода и ближайшие проверяемые шаги. Статусы ниже относятся к исходникам
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
- [x] Внешний поиск pPlay: PR #5 (влит в `dev`) добавил строку поиска из корня,
      список результатов с сортировкой по дате от новых к старым, окно сведений и
      список файлов, Add с ожиданием готовности торрента и открытием его папки, а также
      Android `torrs://` через последнюю активную сеть P2Play.
- [x] Фильтр поиска по выбранным каталогам: pPlay PR #6 добавил UI выбора обычных
      списков и включённых элементов мета-списков, управление всеми типами ввода и
      пустое состояние без каталогов. P2Play PR #18 добавил `lists=<pubkey>[,...]`
      в `GET /api/catalog/search`; фильтр применяется до relevance sort и лимита
      выдачи. Проверки временно создают/удаляют список, покрывают пустой сервер,
      один и несколько ключей и неизвестный ключ.
- [x] Поиск по ключевым словам одной строки: `Mesh::search` нормализует регистр
      (включая кириллицу и `ё/е`), разбивает запрос на слова и требует совпадения
      каждого слова в названии или тегах; совпадение в названии получает больший вес.
      `tools/mesh-test.py` проверяет многословный запрос, совпадения между названием
      и тегами и отсутствие результата, если хотя бы одно слово не найдено.
- [x] Детальная информация записи во внешнем pPlay: PR #7 влит в dev; описания и
      постеры загружаются лениво по подписанным MediaMesh media refs, в фоне, с
      ограничениями размера и кэшированием. Legacy poster URL не используется.
- [x] Серверное хранение прогресса уже реализовано: позиции и длительность раздельны
      по user ID; есть GET /api/progress?since= и Silo-compatible PUT .../progress.
      viewed выставляется на 90% длительности; сервер сохраняет duration только если
      клиент прислал положительное значение.
- [x] Клиентский мост pPlay ↔ P2Play: PR #9 влит в `freetushkan/pPlay/dev`.
      Через нейтральный диспетчер `Io` внешний клиент читает per-user `file_progress`,
      восстанавливает позицию и записывает её через P2Play API; серверная изоляция
      аккаунтов сохраняется. Плеер и MediaProbe не вызывают TorrServer/P2Play напрямую,
      а P2Play-адаптер не использует TorrServer-файлы или `Saved`.
- [x] Длительность и fallback: PR #9 сохраняет известную длительность, не заменяет
      её нулём из mpv и запрашивает FFmpeg MediaProbe, если у P2Play-файла длительность
      неизвестна. Полученная длительность участвует в сохранении серверного прогресса.
      Весь workflow pPlay CI прошёл для Linux, Android, Switch, PS4 и PS5.
- [x] Continue Watching UI во внешнем pPlay: PR #10 влит в `dev`. Список загружается
      через per-user `GET /api/progress`, исключает записи без сохранённой позиции и
      с `viewed=true`, показывает название/файл и время, сохраняет серверный порядок
      по дате обновления и обновляется после возврата из плеера. Выбранная запись открывается
      через обычный P2Play stream path и восстанавливает серверную позицию. В PR также
      устранён безусловный TorrServer flush: общий диспетчер вызывает его только для
      `DeviceType::TorrServe`; P2Play отправляет прогресс только через собственный adapter.
      Linux, Android, Switch, PS4 и PS5 CI прошли.
- [x] C API для встроенного owner password: `p2play_config.owner_password`
      задаёт пароль фиксированной учётной записи `owner` при каждом embedded start.
      Меняется только password hash; owner ID, имя, Silo API token и другие
      пользователи сохраняются. Если поле не передано, standalone сохраняет прежний
      first-login flow. Реализовано и проверено в PR #16.
- [ ] Встроенный режим pPlay: отдельный тип сети `P2Play_embedded` рядом с
      внешним `P2Play`. Встраивать только static `p2play_core` из отдельного
      `git clone` репозитория P2Play в workflow pPlay (с pin конкретного commit);
      standalone daemon не включать в приложение. Внешняя сеть `P2Play` остаётся
      самостоятельным клиентом и не меняет поведение.
      Встроенный core запускается лениво только при выборе `P2Play_embedded`, а при
      переходе в другой модуль останавливается после завершения активных HTTP/stream
      операций. Данные сервера находятся в отдельном каталоге данных pPlay.
      HTTP API оставляет wildcard bind (`INADDR_ANY` для IPv4); не вводить
      loopback-only, interface, subnet или иные LAN-фильтры. Доступность определяют
      маршрутизация, NAT и firewall.
      Все сообщения C API callback встроенного ядра передаются в лог pPlay через
      `Utility::log(LogLevel::Info, ...)` с префиксом `P2Play:`. Они видны в
      `pplay.log` при уровнях Info, Debug и Trace и фильтруются при Off/Error.
      Отдельный `p2play.log` не создаётся.
      Имя встроенного владельца фиксировано: `owner` (ID также `owner`). Пароль
      из настроек `P2Play_embedded` передаётся как `p2play_config.owner_password`
      и принудительно применяется при каждом старте. Меняется только хэш пароля:
      owner ID, API token и дополнительные пользователи сохраняются.
      Встроенный режим должен предоставлять создание дополнительных пользователей
      сервера через интерфейс pPlay и существующий защищённый API.
- [x] Backend users API и Silo rank hierarchy уже реализованы: Owner=100,
      Administrator=50..99, User=10..49 (новый пользователь rank 10), Guest=1..9.
      Управлять можно только аккаунтами с меньшим рангом; owner нельзя удалить или
      понизить. Смена пароля разрешена себе или пользователю ниже рангом.
- [x] pPlay UI управления пользователями во внешней сети P2Play: PR #8 влит в `dev`.
      В настройках сети есть отдельный блок Users с динамическим списком, Add user и
      подменю для пароля, роли и удаления. Owner отображается, но защищён от изменения;
      варианты ролей ограничиваются рангом текущего пользователя. Методы/экран можно
      повторно использовать для `P2Play_embedded`; сброс токена остаётся будущему web UI.
- [ ] Целевые сборки pPlay со встроенным P2Play проходят для Linux, Android, Switch,
      PS4 и PS5; проверены LAN-доступ со второго устройства, запуск/остановка,
      сохранность owner и дополнительных пользователей, а также отсутствие listener
      после деактивации.

### Этап 3: операторский релизёр

- [x] HTTP publishing API уже реализован: создание списка/ключа, NDJSON records,
      media upload, release, snapshot, status, meta-list и shard policy.
- [x] Publisher CLI `p2play publish --import`: dry-run, локальная валидация,
      whitelist, mapping категорий, загрузка описаний/постеров, staging и явный release.
      Приватный ключ остаётся на сервере. Реализовано в PR #27.
- [ ] Версионируемый JSON publisher profile: whitelist, category mapping, source
      enablement/priorities и default/category shard policy. CLI читает профиль;
      source merge остаётся обязанностью Python grabber. GUI, scheduler/threshold
      release, compaction и редактор policy остаются следующими задачами. См.
      `docs/publisher.md`.

- [ ] Ближе к завершению разработки: вывести собственный версионированный HTTP API
      P2Play (например, `/api/v1/...`) и мигрировать pPlay, web UI, embedded и внешние
      интеграции. После этого удалить MatriX и TorrServer.Silo compatibility routes и
      shim; до полной миграции совместимость сохраняется. Это заключительный этап,
      не блокирующий текущую реализацию.

### Этап 5: наполнение каталога

- [ ] Граббер как отдельный инструмент на Python (не часть C++ daemon):
      все источники независимо опциональны; CSV title regex-фильтры, Rutor,
      опциональный RuTracker и Jackett/Torznab; нормализация и merge по приоритету.
      Результат всегда экспортируется в редактируемый NDJSON и отчёт — никакой
      автоматической отправки в publisher. Отдельный режим собирает уникальные
      исходные категории в файлы до mapping/merge для последующей ручной настройки.
      Checkpoints фиксируются только после ручного подтверждения успешно staged
      отредактированного файла. Детали: `docs/grabber.md`.
- [ ] Инкрементально актуализировать недавние раздачи на трекерах через RSS/
      Torznab polling: хранить checkpoint и dedup state, учитывать повторы,
      переупорядоченные/запаздывающие записи и обновление метаданных уже известных
      infohash. RSS — периодический источник, не push-механизм; обновление каталога
      должно быть идемпотентным.
- [ ] Не писать собственные парсеры сайтов трекеров без необходимости. Оценить
      внешний Jackett как агрегатор индексеров и Torznab как стандартный интерфейс:
      capabilities/categories, поиск, выдача недавних публикаций, пагинация,
      ограничения и различия реализаций. Прямые RSS 2.0 источники могут стать
      отдельным простым адаптером. Секреты/API keys хранятся только в локальной
      конфигурации и не попадают в репозиторий или опубликованные каталоги.
- [ ] Репрезентативный большой набор данных и измерения размера сегментов, индекса,
      паков и времени поиска; на их основе уточнить лимиты клиентов.

### Этап 6: необязательные сетевые улучшения — последний этап

- [ ] Исследовать опциональный UPnP / NAT-PMP / PCP для автоматического проброса
      входящих peer/DHT портов на совместимых маршрутизаторах. Это не требование
      MediaMesh и не условие работы DHT: DHT discovery и исходящие соединения уже
      используются без UPnP. Port mapping может улучшить входящую достижимость за
      NAT, но не гарантирует её и зависит от роутера/политики сети. Сначала измерить
      пользу на обычном NAT, затем решить, нужен ли явный opt-in и какие статусы/
      ошибки показывать пользователю. Не включать до завершения основных этапов.
      Справка: [libtorrent UPnP/NAT-PMP settings](https://libtorrent.org/reference-Settings.html)
      и [session port mapping](https://libtorrent.org/reference-Session.html).

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
