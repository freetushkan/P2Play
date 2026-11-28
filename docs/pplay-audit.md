# pPlay ↔ P2Play: аудит паритета и сборки

Дата обновления: 2026-11-28. Это code-review и требования, **не** результат
device acceptance. Текущие refs: P2Play `master`
`f5a70868f791a2e1dfc07654767adde0383d7098`; pPlay `dev`
`23444a68e920329f09c7dbef89ad39c00a18afb2`. Все пути `src/app`, `src/player`,
`src/filer`, `ci/prepare-p2play-embedded.sh` ниже относятся к pPlay, если не указано иное.
Отдельная implementation-ветка pPlay `feature/p2play-library-management`
с commit `1fdebdebe5bde6408923f1f55cd7177aff193a44` содержит первый проход
library/add/delete UI и native API; ветка не является частью `dev`, PR не создавался,
сборка/Actions/device acceptance для неё не подтверждены.

## Вывод

Базовый P2Play client и embedded core реализованы, но полной функциональной
эквивалентности legacy backend нет. Отдельный adapter не использует `previous backend files`,
однако некоторые общие UI/player пути всё ещё вызывают legacy backend напрямую.
Нельзя считать зелёную сборку доказательством паритета поведения.

Публичный Actions API на момент аудита показывает успешный workflow для pPlay
`de03419ad9c9e781de3a670c5d9bbd7cf7cc0414`:
https://github.com/freetushkan/pPlay/actions/runs/45752020074 .
Это не проверка HEAD `402ee80` и не live-тест pPlay → P2Play. В рамках аудита
платформенные бинарники, сеть и интерфейс не запускались; CI не перезапускался.

## Матрица возможностей

| Возможность legacy backend / общий клиент | P2Play | Доказательство / граница |
| --- | --- | --- |
| Идентификация, credentials, TLS policy | Реализовано | `src/net/p2play.cpp`: verifyServer/apiJson, native version + Bearer; общий HTTP transport |
| Просмотр torrent files, preview/add, playback | Реализовано | openTorrent/addTorrent/getDirList/toStreamUrl, `Filer::showP2PlayRecord/openP2PlayLink` |
| Список всех добавленных пользователем torrents | Частично в `dev`; расширено в feature branch | В `dev` уже есть `P2Play::getLibrary` и read-only `.p2play/library` через native `GET /api/v1/library/torrents`, но нет query/category/sort UI. В `feature/p2play-library-management` добавлены q/category/sort/order и фильтр в Filer; ещё не собраны и не приняты на устройствах |
| Удаление torrent из пользовательской библиотеки | Adapter есть, UI отсутствует в `dev`; UI добавлен в feature branch | `P2Play::deleteTorrent` уже вызывал `DELETE /api/v1/torrents/{hash}`, но не был подключён к интерфейсу. В feature branch добавлены manage menu и подтверждение удаления; API снимает только membership текущего пользователя |
| Добавление magnet / .torrent из library UI | Частично в `dev`; расширено в feature branch | Persistent `addTorrentLink` по native POST уже используется существующим catalog/deep-link flow; в `dev` не было ручного add action и multipart upload. Feature branch добавляет ручной magnet/link и multipart `.torrent` (лимит клиента 15 MiB при HTTP body limit core 16 MiB) | 
| Изменение title/category из library UI | Пробел в `dev`; добавлено в feature branch | В feature branch `PUT /api/v1/torrents/{hash}` редактирует общие метаданные infohash; это не per-user display alias. Branch-only, ещё не проверено сборкой |
| Возобновление позиции, запись duration | Реализовано | `Io::get/setPlaybackProgress`, `Player::load/saveProgress`, per-user file_progress |
| Continue Watching | Реализовано | `P2Play::getProgress`, virtual progress path, фильтр viewed/position, refresh при возврате из fullscreen |
| Mark watched / reset file/folder на сервере | Неполный паритет | `Filer::markWatched/markFolder`, reset path в `filer.cpp` используют legacy adapter-specific branch. Для P2Play меняется локальный watch-later, серверная метка/позиция не синхронизируются |
| Значки watched, полосы прогресса, folder marks | Неполный паритет | `filer_item.cpp`, `details_view.cpp`, `media/folder_marks.cpp`: `legacy adapter::isFileViewed/keptPosition/getTree` |
| Локальная копия / offline Saved | Нет нейтрального P2Play аналога | `Io::localCopy` для P2Play возвращает пусто, `touchSavedFile` пропускается. Нельзя подключать legacy backend Saved как обход |
| Deep links | Частично | `torrs://` в `main.cpp` → `openP2PlayLink` с последней внешней P2Play сетью (lastP2PlayNetworkIndex исключает embedded); HTTP stream-link ownership/resolution в `Main::openLink` остаётся legacy backend-only |
| Описания/постеры | Catalog details реализованы | signed refs загружаются лениво в `getCatalogDetails`; это не доказывает паритет обложек файлов/audio screen |
| Audio artwork / shared-player independence | Нарушение границы | `Player::pictureUrlsOf` в `player_visual.cpp` напрямую вызывает `legacy adapter::toStreamUrl/getInfo`; `DetailsInfo::coverOf` использует `Saved::openUrl` |
| Discover, meta-lists, выбор списков поиска | Реализовано, P2Play-specific | `getCatalogLists`, `.p2play/discover/lists/results`; нет оснований требовать аналог в legacy backend |
| Users UI | Реализовано | `getUsers/createUser/...`, меню разрешено и для external, и embedded; сервер проверяет numeric rank |
| Embedded start/drain/stop | Реализовано по коду | `p2play_embedded.cpp`, `Io::forgetNetworks/update/shutdown`; physical acceptance открыта |

Паритет означает одинаковые пользовательские сценарии там, где backend имеет
соответствующие функции; это не копирование всех API legacy backend. Для capability,
которой нет, UI обязан явно скрыть/объяснить действие, а не молча сохранить только
локальное состояние с сообщением о серверном успехе.

### Статус native library management на 2026-11-27

P2Play PR #42 уже закрыл серверный контракт и web UI; merge commit указан выше,
CI jobs успешны (ссылка и границы acceptance — в `docs/roadmap.md` и
`docs/web-ui.md`). В pPlay `dev` на snapshot `23444a68` существовали
read-only library listing, persistent magnet add через catalog/deep link и
функция DELETE adapter, но не были подключены пользовательские library actions,
category/query/sort controls, ручной magnet add, multipart file upload и PUT
для title/category. Первый набор этих изменений внесён в
`feature/p2play-library-management` одним commit; branch сравнивается с `dev`
как 1 commit ahead / 0 behind, но не проверен сборкой. Это не закрывает пункт
roadmap до прохождения CI/проверок клиента.

## Приоритетные дефекты и риски

1. **Секрет в debug log.** `P2Play::toStreamUrl` добавляет `?token=...`, затем
   `Player::load` пишет `effective_url=` в Debug. `Utility::log/writeLog` не
   редактирует message. Это найденный путь раскрытия токена, не только гипотеза.
   Требуется общий URL sanitizer (query token, userinfo, чувствительные параметры)
   до любой записи в log и безопасный stable key для watch-later/cache (смена API token не должна терять resume); тестировать
   synthetic secret, включая ошибки/retry. Core log redaction это не исправляет.
2. **Прогресс блокирует player update.** `saveProgress(false)` вызывается в
   `Player::onUpdate`, P2Play PUT выполняется синхронно через `Http::perform`,
   тогда как legacy backend setter ставит работу в thread. Timeout может заморозить
   интерфейс. Требуются bounded async очередь, coalescing по user/file, порядок
   final write/reset, отмена при смене аккаунта и ограниченный flush перед shutdown.
3. **Reset не равен viewed=false.** P2Play backend (`src/core/api_silo.cpp`)
   имеет POST viewed и PUT progress, но PUT использует keepViewed=true,
   а POST viewed=false сам по себе не обнуляет position. Нужно определить
   последовательность/атомарный контракт reset и тесты; не имитировать его
   единственным PUT position=0. UI marks должны читать тот же server state.
4. **Embedded drain без deadline.** `P2PlayEmbedded::shutdown` ждёт в цикле без
   таймаута; активный LAN stream может удерживать завершение. Это следствие
   текущего требования drain, не доказанная deadlock. Определить grace period,
   отмену/force-stop и видимое состояние, проверить длинный запрос/stream и exit.
5. **Port 8090 жёстко задан.** `config.http_port=8090`, readiness проверяет тот же
   порт. Проверить занятый порт внешним сервером, ошибку start, retry и сохранность
   accounts. Не добавлять скрытый fallback endpoint без согласования с настройками.
6. **Shutdown request не потребляется host.** C API предоставляет
   `p2play_shutdown_requested`, но pPlay lifecycle его не опрашивает. Уточнить
   поддержку `/shutdown` в embedded: host-controlled shutdown или явно
   неподдерживаемое действие, не обещать поведение standalone.
7. **Alternative source path несогласован.** CMake разрешает
   P2PLAY_EMBEDDED_SOURCE_DIR, но `p2play_embedded.cpp` include указывает прямо в
   `../../third_party/P2Play/...`. Исправить target include dirs/header boundary,
   проверить out-of-tree source path, не зависеть от случайной копии checkout.
8. **Документы устарели.** Integration/platform docs называли Continue Watching
   и embedded реализацию будущими и ссылались на старый pin; ниже исправлено.
   Numeric rank presets уже есть; полной capability-модели по-прежнему нет.

Для HTTP redirects отдельно проверить same-origin/cross-origin credentials и
HTTPS downgrade в общем curl transport. Не утверждать утечку Bearer на redirect
без проверки версии libcurl и его правил заголовков.

## Как сейчас выбирается core

- `.github/workflows/main.yml` задаёт P2PLAY_CORE_REF=
  `1f30ceebd253cd4a71e3b978fcf0e8b70ac1f3dd`.
- `ci/prepare-p2play-embedded.sh` fallback — другой SHA,
  `e33886c383cd0e9d9645777ec1ac73a18711a88f`.
- Скрипт fetch-ит `master`, но checkout делает по REF: это **commit pin, не tag**.
  Fetch новой ветки не означает использование нового core.
- `ci/android-build.sh` source-ит тот же скрипт один раз перед сборкой обоих ABI.
- В ручном `.github/workflows/p2play-network.yml` нет prepare/Boost шага, хотя
  CMake теперь безусловно требует embedded source: diagnostic workflow устарел.
- Разница pinned core → текущий master не только docs/tools: изменены
  core.cpp/core.hpp/stream.cpp (TCP/uTP settings, запуск peer traffic после
  назначения wanted pieces). Переключение latest меняет runtime, нужен live test.

## Рекомендуемая политика: latest в dev, точный SHA внутри каждой сборки

Это **предлагаемая реализация**, в данном docs-патче workflow не меняется.

1. Один resolve job до platform jobs получает remote `refs/heads/master` и
   фиксирует полный 40-hex SHA в job output. Не использовать независимо
   разрешаемый `master` в каждом job. Manual input `core_ref` позволяет override.
2. Все platform jobs и оба Android ABI используют только этот SHA. Fetch/checkout
   обязан проверить `rev-parse HEAD == resolved SHA` и detached state. Ветка
   master движется дальше — уже начатая сборка от этого не меняется.
3. Dev/PR могут брать последний master на старте workflow; release/rebuild
   используют зафиксированный lock/build manifest (pPlay SHA, core SHA,
   submodule SHAs, SDK/container/toolchain, Boost patches). Один источник default,
   убрать расхождение workflow env / script fallback. Не требовать правки pin
   для каждой обычной dev сборки.
4. Записать manifest в артефакты/summary и build info. Application version 0.1
   не идентифицирует core commit. При retry/re-run использовать исходный manifest
   или явный override: просто resolve latest заново не воспроизводит старый run.
5. Core push сам по себе сейчас не запускает pPlay workflow. Если нужна такая
   интеграция — отдельный trusted dispatch/schedule с concurrency/cancel-in-progress
   для dev. Не запускать полную матрицу на каждый grabber/docs commit без причины.
6. Не использовать `pull_request_target` для исполнения PR-кода с write token;
   resolver получает только публичные исходники. Fork PR не пишет trusted caches.

## Cache: независимо от pin/latest

Сейчас есть caches static dependencies, Gradle и Boost headers. Compiler cache
P2Play/libtorrent/pPlay не настроен; CMake build dirs удаляются перед configure.
Сохранение одного checkout не заменит кэш компилятора и не решит длительную сборку.

Предлагается **ccache** для C/C++ через CMAKE_C_COMPILER_LAUNCHER и
CMAKE_CXX_COMPILER_LAUNCHER (включая core/libtorrent и Android оба ABI).
Не сохранять/восстанавливать готовую .a по ключу `latest` и не пропускать CMake/build
на cache-hit: заголовки/флаги обязаны проверяться компилятором.

Ключ compiler cache:
`ccache-vN-${platform}-${abi}-${compiler-sdk-container-fingerprint}-${build-flags}-${pplay-sha}-${core-sha}`.
Restore prefix заканчивается до pplay/core SHA, но включает platform/ABI/toolchain/flags:
это позволяет повторно использовать неизменённые translation units после нового
commit, не смешивая PS4/PS5/Switch/Linux/Android. ccache дополнительно проверяет
содержимое исходников/заголовков, compiler identity и command line. Указать
CCACHE_BASEDIR, compiler_check=content, лимит объёма, печатать hit/miss statistics.
Не включать один только branch как immutable cache key: actions/cache не обновит
уже существующий key. Core SHA в key без restore-prefix обнулит межревизионный reuse.

Boost cache key сейчас `p2play-boost-1.86.0-v1`; должен учитывать hash
**выбранного** core `ci/boost-headers.sh` и `patches/boost/**`, вычисленный после
checkout core. Не hashFiles до появления checkout и не только номер Boost.
При смене patchset пересоздавать чистый header tree: старые патчи могут остаться.
Generic download/source caches отделить от compiler outputs. Секреты, signing keys,
пользовательские данные и runtime DB никогда не кешировать.

Мерить cold/warm configure/build/download отдельно на одинаковом toolchain и
двух соседних SHA. Не обещать ускорение линковки, packaging или всех ABI по
одному Linux результату. Ограничить размер caches, разрешить trusted jobs запись,
fork PR — только чтение доступных caches. `:latest` container images требуют
fingerprint SDK/compiler, иначе restore prefix слишком широкий.

## Следующие последовательные PR (не от одной базы одновременно)

1. Документация/аудит в P2Play/master (этот патч), без ложного закрытия parity.
2. pPlay/dev: latest-resolver + one-SHA manifest, единый default, cache headers и
   compiler cache; починить diagnostic workflow и out-of-tree include. Полная
   матрица, cold/warm stats и C API/live smoke с реально выбранным core.
3. pPlay/dev: redact URL log и async progress; тесты account switch, slow/offline
   server, last write on exit, отсутствие secrets в logs/cache paths.
4. pPlay/dev: neutral viewed/reset/folder state и library CRUD. Добавить backend
   методы, не использовать legacy backend Saved или прямые вызовы из player/probe.
5. Отдельно определить offline P2Play storage и stream-link ownership; acceptance
   на реальных Linux/Android/Switch/PS4/PS5 с LAN-вторым клиентом и crash restart.

## Обязательная приёмка

- Одинаковый core SHA на всех jobs/ABI; ручной override, release rebuild без сети
  к branch tip, cache miss/hit и invalidation после SDK/Boost patch/header change.
- C API start/stop/restart, owner password change с сохранением users/token,
  occupied port, rapid network changes, долгий LAN stream во время deactivation.
- Два пользователя: resume/duration/viewed/reset/Continue Watching изолированы;
  ручные marks видны другому клиенту того же user после refresh; folder batch bounded.
- Core details/media lazy и bounded; поиск не загружает media, библиотека/удаление
  не путают torrent membership с catalog subscription.
- Локальные mock HTTP тесты + хотя бы Linux live pPlay/adapter ↔ daemon smoke.
  Наличие standalone core/Silo tests не заменяет pPlay acceptance.


## Повторная проверка 2026-11-27: native library API и pPlay network UX

Проверены P2Play master после merge PR #39
(41baee745326d52ee33e70277345fe14e4ad021c) и pPlay dev
(2f38becc179132256b131194765d5f6b3266295d). Это статический code review,
не физическое воспроизведение на устройстве.

### Новые P2Play library endpoints

P2Play master теперь предоставляет независимый REST namespace:

| Метод | Путь | Назначение |
| --- | --- | --- |
| GET | /api/v1/library/torrents | Пользовательская библиотека, q/category/sort/order/offset/limit |
| GET | /api/v1/library/categories | Категории библиотечных записей |
| POST | /api/v1/torrents | Magnet/link JSON или multipart .torrent |
| GET | /api/v1/torrents/{hash} | Запись из собственной библиотеки |
| PUT | /api/v1/torrents/{hash} | Title/category metadata |
| DELETE | /api/v1/torrents/{hash} | Убрать только membership текущего пользователя |

Native library API использует существующий Silo-compatible login/token auth:
POST /api/auth/login, Bearer token (и same-origin cookie в web UI). Эти auth
routes пока не versioned. Catalog, users, progress и подробный stream/file status
также остаются отдельными уже существующими контрактами. Нельзя считать, что
ответ /api/v1/library/torrents заменяет подробный status/file_stats response:
он содержит поля карточки, а не все данные для воспроизведения.

В pPlay/dev (src/net/p2play.cpp) уже используются старые Silo-shaped routes
для добавления/получения torrent status и API каталога. Library CRUD нужно
перевести на новые пути с отдельным разбором items, total/category и новым
форматом add response; получение file list, file indices, authenticated stream
URL и progress пока не менять механически — сначала сопоставить поля/маршруты.
P2Play MatriX build opt-out не должен выключать native/Silo auth или stream paths.

### Сокращение вариантов сети

Текущая модель pPlay имеет отдельные NetworkType::legacy adapter и
legacy Silo adapter, а также P2Play и P2PlayEmbedded. UI показывает
legacy Matrix adapter, legacy Silo adapter, P2Play, P2Play_embedded.
Discovery в src/net/discovery.cpp ищет хосты в локальном /24 на TCP 8090,
после чего для одного выбора типа проверяет только один маршрут:
MatriX /echo, Silo /api/system/ping (ответ JSON status=ok).
Встроить оба probe в единую проверку одного типа legacy backend; не считать
отсутствие MatriX /echo признаком недоступности Silo. Сканирование сейчас
опирается на один адрес, выбранный route-to-8.8.8.8; многосетевые интерфейсы
и конкретные interface routes ещё не проверены.

Предлагаемая модель настройки:
- один видимый тип legacy backend, dialect определяется успешным уникальным ping;
- один видимый тип P2Play, рядом instance selector: External/Built-in;
- External сохраняет URL, user/password, Bearer login;
- Built-in использует 127.0.0.1:8090, фиксированный user owner и только
  password field, передаваемый через p2play_config.owner_password;
- старые enum/string значения остаются распознаваемыми при чтении config и
  нормализуются при сохранении; существующие URLs, users, passwords, TLS/sync
  settings не теряются.

### Возможная область прерываний legacy backend playback

Наблюдение пользователя: playback через legacy backend иногда прерывается, когда
в filer открыт встроенный P2Play; предполагаемая корреляция — моменты sync.
Причина пока не подтверждена.

Статические участки, которые нужно измерить:
- legacy backend list refresh: previous backend source использует global g_fetchMutex
  для одного fetch одновременно, cache interval networkSync(root), stale
  fallback и отдельные viewed/progress workers.
- Embedded P2Play: p2play_embedded.cpp запускает отдельный core worker,
  останавливает accept на source change и ждёт p2play_active_requests() /
  p2play_active_streams() до graceful stop. Core создаёт libtorrent session.
- HTTP: pPlay Http::perform использует Utility::NetworkLock; сам lock
  блокирует параллельные операции только на Switch, на остальных платформах
  это no-op. Это не доказывает, что lock участвует в mpv stream stalls.
- Player progress: Io dispatch различает legacy backend и P2Play; legacy backend
  progress setter ставит фоновую job, а P2Play setter выполняется через свой
  adapter. Предыдущий unconditional legacy backend flush уже ограничен только
  DeviceType::legacy adapter.

Следующий шаг — instrumentation с monotonic timestamps и bounded regression test:
legacy backend stream active + filer listing embedded library + periodic MediaMesh
sync, сравнить без embedded, embedded idle и embedded loaded. Фиксировать
stream read gaps/reconnects, CPU/network throughput, lock wait times, queue depth,
sync duration и stop/drain transitions. Не считать CPU/network contention доказанной
причиной до воспроизводимого теста.



## Update 2026-11-28: P2Play path restore, saved copies and progress reset

The pPlay integration branch now keeps the P2Play virtual filesystem semantics when restoring
both the last library selection and the last playback session. A virtual media file
(`.p2play/file/<hash>/<index>/<title>/<relative-path>`) is not itself a directory; restore
opens its owning `.p2play/library/<hash>/<title>` directory (or `.p2play/direct/<hash>`
for direct-route files) and then re-selects the exact file. The library listing also accepts
the canonical `.p2play/library/<hash>` parent without a title component and obtains the
display title from the server details.

Offline saved-copy reads now go through the network-neutral `Io::streamUrl` dispatcher.
This prevents `Saved::Source` from accidentally calling the TorrServe URL converter on a
P2Play path. The TorrServe-specific `[all files]` pseudo-item exclusion is likewise now
guarded by the actual device type.

For P2Play file/folder progress reset, the client uses the native
`DELETE /api/v1/torrents/{hash}/files/{id}/progress` operation. This deletes the user's
position and viewed record together; sending only `viewed=false` was insufficient.
Folder mark/unmark now reads and updates P2Play's per-file viewed/progress records as well
as TorrServe state, without routing P2Play requests through TorrServe methods.

These are code changes on the current pPlay PR branch; CI and runtime acceptance are still
required. Watch-later paths continue to be keyed from the stream URL; the server's per-user
API token is durable across ordinary reconnects. Token reset intentionally creates a new
security identity and should not be treated as a transparent credential refresh.


## Web UI refinement (2026-11-28)

- Main navigation remains Library → Playback history → Search; library cards now use inline title editing and save category/title on Enter or blur.
- The library filter endpoint reports both the post-filter result count and total per-user library count; a one-page result hides pagination.
- Duration values use compact unit labels (hours, minutes, seconds) rather than clock notation.
- Theme preference supports light, dark and automatic (OS color-scheme) and is persisted per user.
- Replaced native browser prompts/confirms/alerts with the centered accessible UI dialog; account role is shown under username in the account menu.
- Disk-cache size control is grouped with and hidden by its enable switch; the catalog memory index and public MediaMesh directory settings share a group.
- List names in Lists/Discover are returned by the lists APIs and are displayed as supplied. A specific item name (for example “Media HTTP”) is not a hard-coded UI control; check the runtime list record before removing it.

  
The public MediaMesh directory switch is evaluated during core startup. Changing
`DisableMeshDirectory` therefore returns `restart_required: true`, and the UI labels
the setting accordingly. Lists shown by Lists/Discover remain the records returned by
the catalog APIs; changing the local announcement setting does not delete list records.
