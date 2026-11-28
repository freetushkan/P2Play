# P2Play / MediaMesh — рабочий план

Обновлено: 2026-11-28. Это компактный сопровождаемый план для ветки разработки.
Платформенная матрица и границы Android/Silo описаны отдельно в
`docs/platform-matrix.md`; исходный DOCX в ветке `docs` остаётся справочным.
Исходный большой документ `P2Play_MediaMesh.docx` и его текстовая версия `P2Play_MediaMesh.md`
сохранены в ветке `docs`; этот файл фиксирует состояние кода и ближайшие проверяемые шаги. Статусы ниже относятся к исходникам
и тестам репозитория, а не к проверкам на всех целевых устройствах.

## Готово

### Этапы 0–2: ядро, стример и API

- [x] Общая C++-библиотека ядра и автономный демон; Linux, Android, Switch, PS4 и PS5
      имеют отдельные сборочные обвязки.
- [x] Native P2Play REST API `/api/v1`: авторизация, пользователи, библиотека, прогресс и стриминг.
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
- [x] Discovery до подписки и распространение знаний по сети: при появлении ключа
      из `have` или `known` узел запрашивает и проверяет подписанный BEP 44 pointer,
      кэширует ограниченное имя из существующего `v.n` (до 64 байт) и отдаёт его через
      `GET /api/lists/discover`; pPlay Discover показывает имя до подписки. Discovery-only
      путь не скачивает version torrent, manifest, records или media и не создаёт
      подписку. PR #30 добавил отдельный транзитивный BEP 10 `known` inventory gossip:
      узел передаёт соседям списки, о которых знает сам или узнал от других. `holders`
      считает непосредственных соседей, объявляющих наличие локальной копии через `have`
      (это остаётся недоверенной сетевой подсказкой); `known_by` отдельно показывает
      непосредственных соседей, сообщивших знание через `have` или `known`. Рейтинг использует только `holders`, никогда
      `known_by`. Недоверенные sequence/hints не подменяют проверку подписи; лимиты
      запросов/очередей сохранены. Имя берётся из существующего поля pointer `v.n`;
      отдельный BEP 10 `known` — новый тип сообщения, без изменения структуры pointer.

### Этап 4: клиент и мета-списки

- [x] Обложки торрентов: сервер сам ищет картинку в раздаче (`poster.*`, `cover.*`,
      `folder.*`, …), иначе снимает кадр видео через ffmpeg (`CoverProbe`), кэширует по
      infohash в `<data>/covers/` и отдаёт `GET/POST/DELETE /api/v1/torrents/{hash}/cover`
      с `ETag`/`304`; карточки библиотеки несут `cover`/`cover_source`; постер клиента
      (`poster` в POST/PUT) не заменяется. Извлечение идёт в отдельном потоке, не на
      HTTP/libtorrent. Проверки — `tools/ui-test.py` с локальным сидером и ffmpeg.
      Контракт и границы: `docs/covers.md`. Граница: pPlay-сторона правила «без двойного
      кэша» (`docs/pplay-integration.md` §4b) ещё не реализована; Switch/PS4/PS5 —
      только картинки из торрента, без ffmpeg.
- [x] Ветка `fix/library-title-editing` снова собирается и проходит `tools/ui-test.py`:
      в `Settings` возвращены поля ограниченного дискового кэша и каталожного индекса
      (`diskCacheEnabled`, `diskCacheSize`, `cacheListsInMemory`) и член `m_diskTorrents`,
      которых требовал коммит 2a26240; исправлен битый bencode-фикстура ui-теста
      (`4:name7:one.bin`), браузерный режим переведён с `wait_for_function` (запрещён CSP
      страницы) на опрос DOM. Побочно: `GET …/files` больше не держит запрос 60 с
      (`?wait=`, по умолчанию 15 с), а `restart_required` в настройках выставляется только
      при фактическом изменении значения.
- [x] Веб-интерфейс автономного P2Play: вкладка Library, добавление magnet/link и
      multipart `.torrent`, поиск, категории, сортировка, изменение метаданных и
      удаление с подтверждением. Собственные версионированные маршруты:
      `/api/v1/library/torrents`, `/api/v1/library/categories`,
      `POST /api/v1/torrents` и `GET/PUT/DELETE /api/v1/torrents/{hash}`.
      PR #39 дал базовую вкладку Library; PR #42 завершил управление библиотекой и
      влит в `master` 2026-11-27, merge commit
      `f5a70868f791a2e1dfc07654767adde0383d7098`. Linux/API и платформенные jobs
      P2Play CI ([run 45960270448](https://github.com/freetushkan/P2Play/actions/runs/45960270448))
      и отдельный Silo plugin CI ([run 45960270383](https://github.com/freetushkan/P2Play/actions/runs/45960270383))
      завершились успешно. Граница данных: membership per-user, название/категория —
      общие torrent metadata infohash. Browser/Playwright, реальные Android WebView
      и device checks остаются отдельной эксплуатационной приёмкой.
      Контракты: `docs/web-ui.md`.


- [x] PR #45 removes MatriX/HTTP Basic and the unversioned Silo REST API completely. The compatibility CMake option and compatibility-only tests are removed; the versioned P2Play REST API and Bearer authentication remain. `tools/auth-test.py` asserts that the legacy routes return 404.
- [ ] pPlay/dev: мигрировать и завершить проверку library/add/delete через native API:
      `GET /api/v1/library/torrents`, `GET /api/v1/library/categories`,
      `POST /api/v1/torrents`, `GET/PUT/DELETE /api/v1/torrents/{hash}`.
      Первый implementation commit подготовлен в отдельной ветке
      `freetushkan/pPlay:feature/p2play-library-management` от `dev`
      (`23444a68`), commit `1fdebdebe5bde6408923f1f55cd7177aff193a44`:
      native query/category/sort, magnet и multipart .torrent add, edit title/category,
      per-user delete с подтверждением и library UI. Это пока branch-only work:
      PR нет по договорённости, Actions/build/device acceptance не запускались.
      Перед отметкой done нужны сборка и проверка сценариев на доступных target platforms.
      Не трактовать compact library card как подробный file/stream status.
      Login и уже существующие Catalog/MediaMesh endpoints мигрировать отдельно
      только после добавления их versioned contract.
- [ ] Сократить сетевые типы pPlay: один `TorrServer` с автоопределением MatriX
      по `/echo` и Silo по `/api/v1/system/ping`; один тип `P2Play` с дополнительным
      режимом instance: external или built-in. External показывает URL/user/password
      и сохраняет существующую схему; built-in скрывает URL/user, оставляет пароль
      для фиксированного `owner` и использует embedded lifecycle. Сохранить миграцию
      старых `TorrServer.MatriX`, `TorrServer.Silo`, `P2Play_embedded` settings.
- [ ] Диагностика прерывания TorrServer playback при открытом embedded P2Play в filer:
      добавить timestamps/operation IDs для TorrServer list/viewed sync, P2Play
      embedded start/update и player stalls; воспроизвести на загруженном torrent
      при параллельной работе filer. Проверить contention CPU/network, fetch mutex,
      curl/NetworkLock (особенно Switch) и embedded session activity. Добавить
      regression stress test; не менять sync cadence/locks без измеренного источника.
- [x] Подготовлен статический code-review pPlay/dev `402ee80` относительно
      P2Play/master `052d569`: матрица паритета, реальные пробелы, расхождение core
      pins и план кэширования. `docs/pplay-audit.md`. Это исторический срез аудита,
      не актуальный runtime/device acceptance и не доказательство функционального паритета.
- [x] pPlay workflow разрешает latest P2Play master один раз на run и использует
      один immutable SHA во всех platform jobs/Android ABI; checkout проверяет SHA.
      Boost headers кэшируются с учётом core SHA.
- [ ] pPlay workflow: build manifest для release/re-run, compiler cache (ccache)
      с ключом platform/ABI/toolchain/flags, полный fingerprint SDK/container и
      проверка diagnostic workflow. Не подменять сборку восстановлением готовых
      бинарников из mutable cache.
- [ ] pPlay/dev: устранить token URL в Debug log и синхронную запись прогресса
      из player update; безопасная async очередь/flush/reset/account switch и тесты.
- [ ] Паритет TorrServer: server watched/reset, file/folder marks, нейтральный
      artwork path, library list/delete; отдельно согласовать P2Play offline storage
      и stream-link ownership. Не переиспользовать TorrServer Saved/adapter files.
      Для обложек: pPlay принимает `cover`/`cover_source` и `GET …/cover` вместо прямого
      похода в `TorrServe` за картинкой; во встроенном режиме читает
      `<pPlay data>/p2play_embedded/covers/<infohash>.img` и не копирует файл в
      `data/cache/posters` (`docs/pplay-integration.md` §4b).
- [ ] Embedded acceptance: busy port, rapid activation, bounded graceful shutdown,
      LAN stream во время drain, owner/users persistence и host shutdown contract.

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
      `GET /api/v1/system/version` возвращает `{"name":"P2Play","version":"0.1"}` без
      MatriX/TorrServer/Silo compatibility prefixes. Версия берётся из единственного
      CMake VERSION_MAJOR/MINOR источника. Endpoint работает без токена; legacy
      `/api/v1/system/ping`, `/api/v1/system/version` и MatriX `/echo` не изменены.
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
- [x] Встроенный режим pPlay реализован в `freetushkan/pPlay/dev`: отдельная сеть
      `P2Play_embedded` рядом с внешним `P2Play`; в приложение линкуется только static
      `p2play_core`, standalone daemon не включается. Workflow pPlay клонирует P2Play
      и checkout-ит закреплённый commit из `ci/prepare-p2play-embedded.sh`.
      Аудит 2026-11-26: pPlay/dev `402ee80` использует в main workflow core
      `1f30ceeb...`, fallback prepare script — `e33886c...`; это расхождение
      и план latest-on-dev / one resolved SHA описаны в `docs/pplay-audit.md`.
      Core запускается лениво при выборе embedded-сети и останавливается после drain
      активных HTTP/stream операций при переключении модуля. Используется отдельный
      каталог данных pPlay. HTTP API сохраняет wildcard bind (`INADDR_ANY` для IPv4),
      без loopback/interface/subnet/LAN-фильтров. C API callback логируется через
      `Utility::log(LogLevel::Info, ...)` с префиксом `P2Play:` в `pplay.log`;
      отдельный `p2play.log` не создаётся. Фиксированный владелец — `owner`;
      пароль настроек передаётся через `p2play_config.owner_password` при каждом старте,
      без сброса owner ID, API token и дополнительных пользователей. Для управления
      пользователями используется защищённый users API и клиентский UI pPlay.
      Реализация: `src/net/p2play_embedded.cpp`, `CMakeLists.txt`,
      `ci/prepare-p2play-embedded.sh` в pPlay. Исторический `dev` commit
      `61e9e8513b6f1f2e87af2c873b1cfceb49fc66bd` прошёл полный CI run #1475.
- [x] Backend users API и Silo rank hierarchy уже реализованы: Owner=100,
      Administrator=50..99, User=10..49 (новый пользователь rank 10), Guest=1..9.
      Управлять можно только аккаунтами с меньшим рангом; owner нельзя удалить или
      понизить. Смена пароля разрешена себе или пользователю ниже рангом.
- [x] pPlay UI управления пользователями во внешней сети P2Play: PR #8 влит в `dev`.
      В настройках сети есть отдельный блок Users с динамическим списком, Add user и
      подменю для пароля, роли и удаления. Owner отображается, но защищён от изменения;
      варианты ролей ограничиваются рангом текущего пользователя. Методы/экран можно
      повторно использовать для `P2Play_embedded`; сброс токена остаётся будущему web UI.
- [x] Встраиваемый core включён в CI-сборки pPlay для Linux, Android, Switch,
      PS4 и PS5; workflow также собирает PS5 PKG. Полный pPlay CI run #1475 прошёл
      на `dev` commit `61e9e8513b6f1f2e87af2c873b1cfceb49fc66bd`.
- [ ] Эксплуатационная приёмка `P2Play_embedded` на реальных устройствах: LAN-доступ
      со второго устройства, запуск/остановка при переключении модулей, сохранность
      owner и дополнительных пользователей, отсутствие listener после деактивации.
      Зелёная кроссплатформенная CI-сборка подтверждает компиляцию/линковку, но не
      заменяет проверку маршрутизации, firewall и lifecycle на физическом оборудовании.

### Этап 3: операторский релизёр

- [x] HTTP publishing API уже реализован: создание списка/ключа, NDJSON records,
      media upload, release, snapshot, status, meta-list и shard policy.
- [x] Publisher CLI `p2play publish --import`: dry-run, локальная валидация,
      whitelist, mapping категорий, загрузка описаний/постеров, staging и явный release.
      Приватный ключ остаётся на сервере. Реализовано в PR #27.
- [x] Версионируемый JSON publisher profile: whitelist, category mapping, source
      enablement/priorities и default/category shard policy. CLI читает профиль;
      source merge остаётся обязанностью Python grabber. Реализован и влит в master
      в PR #28 (2026-11-24). GUI, scheduler/threshold release, compaction и редактор
      policy остаются отдельными операторскими задачами. См. `docs/publisher.md`.

- [x] Собственный версионированный P2Play REST API начат в PR #39: библиотека и
      torrent CRUD расположены под `/api/v1`, отделены от MatriX compatibility router
      и проходят сборку с `compatibility flags are absent`. Это ещё не миграция всего API.
- [x] Версионировать собственный P2Play API: Catalog, progress, users, library,
      torrent CRUD, share/stream contracts находятся под `/api/v1`; MatriX,
      HTTP Basic и unversioned Silo compatibility routes удалены в PR #45.
      Миграция внешнего pPlay клиента отслеживается отдельно (pPlay PR #17; merge
      ещё не выполнен).

### Этап 5: наполнение каталога

- [x] Восстановлен вывод прогресса CLI граббера в stderr каждые 10 секунд:
      оценка общего процента, текущий этап/источник и ETA по измеримым этапам.
      См. `docs/grabber.md`; stdout остаётся JSON.

- [x] Inventory report: единая Category-сводка, NDJSON bytes и оценка zlib для
      всех источников; отключённые/пустые источники скрыты в HTML, JSON сохраняет
      диагностику; Anime выделен для Rutor/RuTracker и первой CSV-группы в профиле.
      `tools/grabber-test.py` входит в Linux CI. См. `docs/grabber.md`. 
- [x] Inventory: сводка исходных → конечных полей CSV/Rutor/RuTracker в JSON
      и под катом HTML; явно отмечены provenance и отключённые поля. Добавлен
      `tools/profiles/catalog-inventory.example.json` с Lossy/Lossless и жанрами
      RuTracker, разделением Movie UHD/HD/DVD/Other и осторожной CSV-классификацией.
      Локальные тесты пройдены; выводы и команда прогона — в `docs/catalog-layout.md`.
- [x] Уточнён план inventory: нативные категории Rutor сохранены, видео
      RuTracker приведено к той же схеме, CSV измеряет UHD/4K и музыкальные
      расширения эвристически. Для категорий и плановых шардов добавлено
      сравнение NDJSON/zlib с локальными допполями и без них. Ошибки CSV
      видны в HTML. См. `docs/catalog-layout.md`.
- [x] CSV можно скачать напрямую по HTTPS без Git-клона; обновлён пример
      профиля для сетевых CSV и Rutor. RuTracker остаётся локальным XML/XZ.
- [x] `path` имеет приоритет над URL и Git для всех встроенных адаптеров;
      RSS/Torznab можно читать из сохранённого XML без сетевых запросов.
- [ ] Повторить inventory на исходниках, разобрать degraded, измерить реальные
      размеры шардов и проверить маршрутизацию и лимиты списков до публикации.
- [ ] Production classification: проверить категории на свежем полном inventory
      и на выборках источников; отчётный расчёт не заменяет экспертную классификацию.

- [x] Компактный inventory layout: deleted inventory, profile-controlled
      extra_fields, forum-ID category groups, отдельные Video/Music правила,
      N×N source coverage, Category/Other, NDJSON bytes и оценка zlib. Отчёт и
      fixtures проверяются `tools/grabber-test.py` в Linux CI.
- [ ] Каталог/публикация: production пересборка на свежей базе; точная классификация
      DocSeries и неоднозначных форумов; проверка групп на живых выборках; физический
      экспорт двух CSV каталогов и окончательный выбор одного списка с shard policy
      или двух независимых списков. Ограничения: `docs/catalog-layout.md`.

- [x] Отдельный Python Grabber реализован в `tools/grabber.py`, `grabber_lib.py`,
      `grabber_adapters.py` и `grabber_templates.py`. Источники независимо включаются
      через JSON profile: torrents-csv-data, Rutor, RuTracker XML/XZ, Jackett/Torznab,
      прямой RSS и безопасные JSON/HTML/text templates. Нормализация, приоритетный
      merge, quarantine, ручные overrides/suppressions и SQLite state разделены
      с publisher. Экспортирует полный `catalog-full.ndjson` и производный
      `catalog-delta.ndjson`; неизменённый full экспортируется дельтой, изменённый
      требует полного `--replace`. Pending run и явный ACK связывают хэши файлов,
      режим публикации, accepted count и checkpoints; сам Grabber никогда не
      запускает publisher. Fixtures и `tools/grabber-test.py` проходят Linux CI.
      Контракт и операторский workflow: `docs/grabber.md` и `docs/publisher.md`.
- [x] Инкрементальный RSS/Torznab polling реализован: отдельные checkpoint и
      overlap/dedup state на каждый feed/indexer, pagination, повторная обработка
      запоздавших элементов, per-source priority и изоляция отказов. Изменения
      известных infohash проходят через merge в полные delta-записи; повторы
      идемпотентны, отсутствие старой записи в окне RSS не считается tombstone.
      Direct RSS не требует Torznab caps. Локальные mock-тесты проверяют здоровый
      feed рядом с упавшим, capabilities, redirects и replay; `grabber-test.py`
      включён в Linux CI. См. `docs/grabber.md`.
- [x] Инкрементальный transfer path MediaMesh и полная замена записи по тому же
      infohash реализованы в PR #29: выбранные immutable record segments повторно
      используются только после проверки размера и SHA-256; libtorrent затем
      проверяет байты по v2 torrent. Полная замена публикует атомарный новый snapshot
      под тем же публичным ключом. Сценарии reuse/replacement входят в Linux CI.
- [x] Оценён и использован внешний Jackett через Torznab вместо tracker-specific
      HTML scrapers. Adapter получает capabilities/categories, опрашивает каждый
      indexer независимо, поддерживает pagination, overlap, limits и отдельные
      API keys в локальном профиле; redirects запрещены, ключи не попадают в логи.
      Direct RSS реализован отдельным режимом того же bounded XML/RSS parser.
      Jackett остаётся внешней необязательной службой, не vendored dependency.
      Детали: `docs/grabber.md`.
- [ ] Обложки, остаток: лимит и очистка каталога `covers/`, срок жизни отметки `.none`,
      ручной выбор файла-обложки и загрузка своей картинки в API, кадр для MP4 без
      `faststart`, ffmpeg на консолях. См. `docs/covers.md` («Что не сделано»).
- [x] Первый измеренный large-catalog baseline: 20 000 выбранных записей,
      lazy media packs, compressed segments, search latency, cold start и RSS.
      Профиль входит в Linux CI; метрики и границы измерений — `docs/media-shards.md`.
- [ ] Расширить выборку до production-масштаба (включая 1.8 млн записей) и сравнить
      один каталог с shard selection против двух независимых списков; по результатам
      уточнить клиентские лимиты. Первый 20k baseline не доказывает масштабирование
      на полный inventory.

### Этап 6: проверка сетевой достижимости и port mapping — поздний этап

- [x] Автоматический port mapping уже включён в обычном режиме P2Play: core активирует
      UPnP и NAT-PMP по умолчанию, если mapping не отключён настройкой
      `DisableUPNP`. В закреплённом libtorrent commit
      `9d7443f467147d1784fb7516d2a882db1abb5a8b` NAT-PMP mapper сначала пробует PCP,
      затем использует NAT-PMP fallback. Это относится к входящим BitTorrent/DHT
      соединениям и помогает другим узлам подключаться к клиентам за NAT, которые
      держат MediaMesh-данные. Для HTTP API проброс портов не нужен: он слушает
      wildcard interface, а удалённая доступность определяется маршрутизацией и
      firewall. Исходящие DHT/peer соединения работают и без port mapping.
      В console-режиме и SOCKS proxy deployment mapping намеренно отключается.
- [ ] Проверить port mapping на реальных совместимых маршрутизаторах: успешное
      создание/удаление mapping, обновление lease после смены интерфейса или IP,
      корректную работу при отключённом UPnP/NAT-PMP и поведение за CGNAT/сетями,
      где входящие соединения запрещены. Port mapping не гарантирует достижимость
      через CGNAT и не должен считаться условием работы самого MediaMesh.
      Справка: [libtorrent UPnP/NAT-PMP settings](https://libtorrent.org/reference-Settings.html),
      [session port mapping](https://libtorrent.org/reference-Session.html) и
      [pinned NAT-PMP/PCP mapper](https://github.com/arvidn/libtorrent/blob/9d7443f467147d1784fb7516d2a882db1abb5a8b/src/natpmp.cpp).

## Этап 5: native API, внешние плееры и HTTPS (PR #45)

Реализация идёт в `feature/native-api-silo-ssl` от `master`, PR #45 открыт.
Статусы [x] означают реализацию и проверяемые тесты в этой ветке, а не факт
merge. pPlay PR #17 также открыт; его embedded callback использует существующий
pPlay HTTP/TLS transport. Silo plugin остаётся поисковым адаптером и передаёт
выбранный magnet в Silo через host API; HTTPS transport ему не добавляется.

- [x] Перевести web-ui и pPlay client adapter на версионированный `/api/v1`
      (P2Play PR #45, pPlay PR #17); соответствующие PR остаются открыты до merge.
- [x] Исправить маршруты Linux-теста `Live Silo MediaMesh playback`: внешний Silo
      использует `/api`, P2Play — `/api/v1`. Проверка готовности проходит на
      локальном HTTP-стенде; после патча пользователь подтвердил успешную сборку.
- [x] Удалить MatriX и старый unversioned Silo REST API, HTTP Basic и связанные
      сборочные опции/тесты. Сохранить `torrs://` и `.torrs` как форматы Android/pPlay.
- [x] Один постоянный API-токен на пользователя; повторный login его не меняет,
      явный reset отзывает старые ссылки и Bearer-сессии. Проверяется
      `tools/auth-test.py`, включая сохранение токена после рестарта.
- [x] Web UI копирует share URL как для playable file, так и для всего торрента;
      ссылки используют per-user token и отзываются через explicit token reset.
      Проверки: `tools/ui-test.py` и `docs/native-api-security.md`.
- [x] Silo plugin ищет только в подписанных MediaMesh-каталогах и передаёт
      выбранный magnet непосредственно в Silo через host API.
      Списки обнаруживаются и добавляются по публичному ключу в разделе Lists.
- [x] Ветка поддерживает вложенный HTTP-префикс. `tools/ui-test.py` проверяет
      404 без полного префикса и URL потока с префиксом.
- [x] Android открывает `magnet:` и `torrs://` и отправляет исходную ссылку в
      native API; карточку и постер разбирает сервер. Проверка APK на устройстве
      ещё не выполнена. Web UI уже поддерживает добавление карточек и редактирование
      названия, категории и постера.
- [x] Исправить распознавание PNG и не вызывать HTTP-колбэк без проверенного IPv4.
- [x] HTTPS URL-постеров на всех платформах: Linux — системное CA-хранилище через
      libcurl; Android — системный CA store через аутентифицированный loopback
      SSLSocket bridge; Switch — доверенные CA из libnx SSL service; PS4/PS5 —
      bundle `data/ca/cacert.pem`. pPlay embedded переиспользует pPlay libcurl и
      сетевую TLS preference. Проверка цепочки и hostname включена по умолчанию.
      Redirect без auto-follow, повторная SSRF/DNS проверка каждого hop, downgrade
      HTTPS→HTTP запрещён. Тест: `tools/https-test.py` (CA/hostname/pin/redirect/size).
- [x] Регрессионные проверки token persistence/reset (`auth-test.py`), share links,
      non-persistent preview/base-path (`ui-test.py`), Silo native magnet hand-off
      (Silo plugin/live harness) и HTTPS certificate/hostname/DNS pin/redirect/body
      limits (`https-test.py`). Пяти-платформенный CI — обязательный merge gate PR #45.

Контракт: `docs/native-api-security.md`. Источник TLS для embedded mode:
`pPlay/dev`, `src/net/net_http.cpp`, `Io::tlsVerify()`.

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
