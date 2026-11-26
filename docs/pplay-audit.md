# pPlay ↔ P2Play: аудит паритета и сборки

Дата: 2026-11-26. Это code-review и требования, **не** результат device acceptance.
Проверены P2Play `master` `052d569` и pPlay `dev`
`402ee80a6c7271622659abefb390ab4999ca9009`. Все пути `src/app`, `src/player`,
`src/filer`, `ci/prepare-p2play-embedded.sh` ниже относятся к pPlay, если не указано иное.

## Вывод

Базовый P2Play client и embedded core реализованы, но полной функциональной
эквивалентности TorrServer нет. Отдельный adapter не использует `torrserve.*`,
однако некоторые общие UI/player пути всё ещё вызывают TorrServer напрямую.
Нельзя считать зелёную сборку доказательством паритета поведения.

Публичный Actions API на момент аудита показывает успешный workflow для pPlay
`de03419ad9c9e781de3a670c5d9bbd7cf7cc0414`:
https://github.com/freetushkan/pPlay/actions/runs/45752020074 .
Это не проверка HEAD `402ee80` и не live-тест pPlay → P2Play. В рамках аудита
платформенные бинарники, сеть и интерфейс не запускались; CI не перезапускался.

## Матрица возможностей

| Возможность TorrServer / общий клиент | P2Play | Доказательство / граница |
| --- | --- | --- |
| Идентификация, credentials, TLS policy | Реализовано | `src/net/p2play.cpp`: verifyServer/apiJson, native version + Bearer; общий HTTP transport |
| Просмотр torrent files, preview/add, playback | Реализовано | openTorrent/addTorrent/getDirList/toStreamUrl, `Filer::showP2PlayRecord/openP2PlayLink` |
| Список всех добавленных пользователем torrents | Пробел | Нет GET `/api/torrents` в adapter; корень содержит search/progress/discover/lists, не Library. Уже добавленный torrent доступен через direct path, это не полноценный library browser |
| Удаление torrent из пользовательской библиотеки | Пробел | В adapter нет DELETE torrent; не путать с deleteUser или удалением локальной копии |
| Возобновление позиции, запись duration | Реализовано | `Io::get/setPlaybackProgress`, `Player::load/saveProgress`, per-user file_progress |
| Continue Watching | Реализовано | `P2Play::getProgress`, virtual progress path, фильтр viewed/position, refresh при возврате из fullscreen |
| Mark watched / reset file/folder на сервере | Неполный паритет | `Filer::markWatched/markFolder`, reset path в `filer.cpp` используют TorrServe-specific branch. Для P2Play меняется локальный watch-later, серверная метка/позиция не синхронизируются |
| Значки watched, полосы прогресса, folder marks | Неполный паритет | `filer_item.cpp`, `details_view.cpp`, `media/folder_marks.cpp`: `TorrServe::isFileViewed/keptPosition/getTree` |
| Локальная копия / offline Saved | Нет нейтрального P2Play аналога | `Io::localCopy` для P2Play возвращает пусто, `touchSavedFile` пропускается. Нельзя подключать TorrServer Saved как обход |
| Deep links | Частично | `torrs://` в `main.cpp` → `openP2PlayLink` с последней внешней P2Play сетью (lastP2PlayNetworkIndex исключает embedded); HTTP stream-link ownership/resolution в `Main::openLink` остаётся TorrServer-only |
| Описания/постеры | Catalog details реализованы | signed refs загружаются лениво в `getCatalogDetails`; это не доказывает паритет обложек файлов/audio screen |
| Audio artwork / shared-player independence | Нарушение границы | `Player::pictureUrlsOf` в `player_visual.cpp` напрямую вызывает `TorrServe::toStreamUrl/getInfo`; `DetailsInfo::coverOf` использует `Saved::openUrl` |
| Discover, meta-lists, выбор списков поиска | Реализовано, P2Play-specific | `getCatalogLists`, `.p2play/discover/lists/results`; нет оснований требовать аналог в TorrServer |
| Users UI | Реализовано | `getUsers/createUser/...`, меню разрешено и для external, и embedded; сервер проверяет numeric rank |
| Embedded start/drain/stop | Реализовано по коду | `p2play_embedded.cpp`, `Io::forgetNetworks/update/shutdown`; physical acceptance открыта |

Паритет означает одинаковые пользовательские сценарии там, где backend имеет
соответствующие функции; это не копирование всех API TorrServer. Для capability,
которой нет, UI обязан явно скрыть/объяснить действие, а не молча сохранить только
локальное состояние с сообщением о серверном успехе.

## Приоритетные дефекты и риски

1. **Секрет в debug log.** `P2Play::toStreamUrl` добавляет `?token=...`, затем
   `Player::load` пишет `effective_url=` в Debug. `Utility::log/writeLog` не
   редактирует message. Это найденный путь раскрытия токена, не только гипотеза.
   Требуется общий URL sanitizer (query token, userinfo, чувствительные параметры)
   до любой записи в log и безопасный stable key для watch-later/cache (смена API token не должна терять resume); тестировать
   synthetic secret, включая ошибки/retry. Core log redaction это не исправляет.
2. **Прогресс блокирует player update.** `saveProgress(false)` вызывается в
   `Player::onUpdate`, P2Play PUT выполняется синхронно через `Http::perform`,
   тогда как TorrServer setter ставит работу в thread. Timeout может заморозить
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
   методы, не использовать TorrServer Saved или прямые вызовы из player/probe.
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
