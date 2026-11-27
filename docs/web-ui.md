# Веб-интерфейс P2Play: библиотека и паритет TorrServer

## Цель и границы

Служебного экрана MediaMesh недостаточно для автономного медиаплеера. В /ui/
нужно управлять собственной библиотекой без внешнего pPlay/Silo. Общий устаревший
план из old_docs/P2Play_MediaMesh.md не ограничивает этот объём.

Одна копия HTML/JS/CSS остаётся общей с Silo-плагином. Экран MediaMesh доступен
обоим host-режимам, но вкладка Library и вызовы P2Play library API существуют
только при запуске core-host. В Silo библиотекой управляет сам Silo. Нельзя
направлять запросы к библиотеке P2Play из Silo-host или выдавать библиотеку
служебного daemon token за библиотеку вошедшего пользователя Silo.

## Нативный API P2Play

Самостоятельный версионированный REST API расположен под /api/v1 и реализован
в src/core/api_p2play.cpp. Он не зависит от TorrServer MatriX compatibility
layer и продолжает работать при сборке с P2P_COMPAT_MATRIX=OFF.

| Метод и путь | Назначение |
|---|---|
| GET /api/v1/library/torrents | Вся библиотека пользователя; параметры q, category, sort=date/title/size, order=asc/desc, offset, limit |
| GET /api/v1/library/categories | Категории, встречающиеся в библиотеке пользователя |
| POST /api/v1/torrents | Добавление magnet / поддерживаемой ссылки (JSON) или .torrent (multipart field file) |
| GET /api/v1/torrents/{hash} | Карточка и подробный status/file_stats/viewed/progress |
| PUT /api/v1/torrents/{hash} | Изменение title и/или category |
| DELETE /api/v1/torrents/{hash} | Удаление из библиотеки текущего пользователя |
| GET /api/v1/torrents/{hash}/files | Список файлов, watched и playback progress; `?wait=<0..120>` ограничивает ожидание метаданных (по умолчанию 15 с) |
| GET /api/v1/torrents/{hash}/cover | Обложка торрента (картинка из раздачи или кадр видео); `ETag`, `304`, `X-P2Play-Cover` |
| POST /api/v1/torrents/{hash}/cover | Извлечь/обновить обложку сейчас; `409`, если постер задан клиентом |
| DELETE /api/v1/torrents/{hash}/cover | Убрать закэшированную обложку и отметку об отсутствии |
| PUT /api/v1/torrents/{hash}/viewed | Отметить все файлы раздачи watched/unwatched |
| DELETE /api/v1/torrents/{hash}/progress | Сбросить весь watched/progress state раздачи |
| PUT /api/v1/torrents/{hash}/files/{index}/viewed | Watched mark отдельного файла |
| PUT/DELETE /api/v1/torrents/{hash}/files/{index}/progress | Сохранить позицию или сбросить watched/позицию файла |
| PUT /api/v1/torrents/{hash}/folders/viewed | Watched mark всех файлов внутри torrent-relative folder (path в JSON) |
| DELETE /api/v1/torrents/{hash}/folders/progress?path=... | Сброс watched/progress файлов папки |
| GET /api/v1/torrents/{hash}/files/{index}/stream | Range-capable stream с cookie/Bearer auth |
| POST /api/v1/torrents/{hash}/wake | Активировать раздачу для streaming/metadata |
| GET /api/v1/torrents/{hash}/cache, /peers | Диагностика RAM cache, readers и peer connections |
| GET /api/v1/progress | Per-user Continue Watching |
| GET/PUT /api/v1/settings | Настройки движка; PUT только owner |
| GET/POST /api/v1/users и /api/v1/users/{id} | Управление пользователями с Silo rank checks |
| GET /api/v1/system/stats | Uptime, sessions, readers и глобальная cache статистика |
| GET /api/v1/library/export, POST /api/v1/library/import | Перенос пользовательской библиотеки .torrs |

POST добавляет запись в библиотеку по умолчанию. JSON-поля: link, title,
category, poster. Для multipart используются поля file, title, category, poster; файл и
ссылка одновременно недопустимы, пустой файл отклоняется. `poster` — постер клиента:
он сохраняется как есть и исключает серверное извлечение обложки (см. `docs/covers.md`).
PUT /api/v1/torrents/{hash} принимает title, category и poster; пустой poster
освобождает торрент для серверной обложки. Upload limit нативного
маршрута ограничен HTTP body limit core: 16 MiB вместе с multipart framing;
файл должен быть немного меньше из-за multipart boundary и полей.

Поиск разбивает запрос на слова, требует совпадения каждого слова в title,
infohash или category и использует общую Unicode-fold функцию MediaMesh
(включая кириллицу и ё/е). Категории: все (category не задана), без категории
(category=none) или точное значение без учёта регистра. Фильтрация и сортировка
производятся по всей библиотеке до offset/limit. Сортировка детерминирована
infohash как tie-breaker.

Карточка библиотеки содержит `poster` (постер клиента, если был), `cover` (есть ли
серверная обложка в кэше) и `cover_source` (`torrent:<файл>` или `probe`). Байты
обложки в список не попадают — их отдаёт отдельный маршрут `…/cover`.

Удаление снимает только привязку данного пользователя. Если другой пользователь
сохранил тот же infohash, его библиотека остаётся нетронутой. Категория и название
относятся к общим метаданным torrent infohash, поэтому изменение категории видно
всем пользователям, сохранившим эту раздачу.

## Авторизация и совместимость

Нативный API использует существующую Silo-схему авторизации core: HttpOnly
silo_token cookie для same-origin web UI и Bearer token для API-клиентов.
Токен не хранится в JavaScript/localStorage и не передаётся в URL. Login route
остаётся POST /api/auth/login; Silo-compatible /api/auth/me подтверждает
сессию. Потоковые и остальные защищённые /api маршруты используют ту же
cookie/Bearer авторизацию. Старые TorrServer MatriX маршруты и их HTTP Basic Auth
не переименованы и не изменены; их можно выключить отдельной опцией сборки.

Общий HTML/JS/CSS обслуживается и в Silo-host, но library tab намеренно скрыт там.
Silo не проксирует маршруты /api/v1 и не получает доступ к чужой P2Play-библиотеке.

## Web UI

- Вкладка Library доступна только в core-host.
- Добавление magnet/link или binary .torrent, optional title/category.
- Поиск по title/hash/category, category selector с «все» / «без категории» /
  реальными значениями, сортировка date/title/size в обе стороны.
- Изменение display title и категории, подтверждаемое удаление и импорт/экспорт
  .torrs; общие torrent metadata отделены от per-user library membership.
- Обложка в строке библиотеки (`<img>` на `…/cover`) и в деталях раздачи, с кнопкой
  «Refresh cover» (`POST …/cover`) и подписью источника. Без обложки рисуется
  placeholder, а не битая картинка; внешние постеры не вставляются (CSP `img-src`).
- Детали раздачи: список файлов, проигрывание через защищённый Range stream,
  watched/unwatched, reset позиции и watched для файла, папки или всей раздачи.
- Continue Watching, server stats, owner engine settings и rank-aware user
  management (создание, смена роли, ban, password reset, token regeneration,
  удаление).
- Peer list и RAM/disk cache diagnostics (включая активные reader windows).
- Список файлов и диагностика в деталях раздачи загружаются вне общего флага
  занятости страницы и с `?wait=5`: торрент без метаданных показывает понятное
  сообщение, а не блокирует вкладки на минуту.
- Настройки cache представлены переключателями, числовыми полями и ползунками.
  Ограниченный дисковый кэш (`DiskCacheEnabled`, `DiskCacheSize` в ГиБ) и
  `CacheListsInMemory` выведены в отдельный блок формы сервера; извлечение обложки
  через ffmpeg отключается переключателем `CoverProbe`.
  Изменения применяются на лету; уже открытый torrent сохраняет свой storage
  backend до закрытия, новые активации используют выбранный режим.
- DiskCacheEnabled включает отдельный ограниченный кэш в data/torrent-cache,
  DiskCacheSize задаёт общую квоту (256 MiB–2 TiB). При переполнении удаляются
  самые давно использовавшиеся закрытые торренты; активные потоки не удаляются.
  Это кэш, а не пользовательская папка загрузок. UseDisk остаётся legacy
  unlimited disk mode и взаимоисключается с ограниченным disk cache.
- CacheListsInMemory отключает постоянно удерживаемый в RAM индекс записей
  MediaMesh. Подписанные manifests и segments всегда хранятся на диске;
  поиск/карточки читают их с диска при запросе. В браузере полный список
  каталогов и раньше не кэшировался локально.
- Runtime settings применяются немедленно; legacy UseDisk, save path, listen
  port и DHT bootstrap помечаются как требующие restart.
- Ошибка не очищает форму; операции блокируются на время запроса.
- Недоверенные строки выводятся через textContent; CSP разрешает media-src
  только same-origin для защищённого stream; внешние постеры/HTML не вставляются.

## Проверки и дальнейший паритет

tools/ui-test.py проверяет защищённый API, добавление magnet и multipart
torrent, поиск/категории/сортировку, удаление и изоляцию пользовательских
библиотек; file list, watched/progress/reset, Continue Watching, stream index
validation, peers/cache diagnostics, owner settings, disk-cache/catalog cache
modes, обложки (`docs/covers.md`) и users API. Browser режим
проверяет login, добавление/редактирование torrent, обложку и её refresh,
details/files, блоки storage/catalog cache в настройках сервера, users и Continue
Watching через Playwright. В режиме `--browser` страница ждётся опросом DOM:
CSP страницы (`script-src 'self'`, без `unsafe-eval`) запрещает
`wait_for_function`, поэтому проверки ждут появления текста и снятия флага
`aria-busy` из Python. Linux CI запускает ui-test.py без `--browser`; браузерный
режим — отдельная ручная проверка.

Этот этап закрывает основные web-ui операции управления библиотекой и
воспроизведением, но не заявляет абсолютный TorrServer parity: остаются
сравнительные проверки редких BTSets, выбор приоритетов отдельных pieces,
совместимость внешних MatriX/Silo клиентов и приёмка Android WebView/embedded
LAN/целевых устройств. Browser CI на Linux не заменяет эти проверки.

## Состояние реализации

PR #42 `web-ui: complete torrent library and server management` влит в
`master` 2026-11-27 (merge commit
`f5a70868f791a2e1dfc07654767adde0383d7098`). PR CI: [P2Play workflow
run 45960270448](https://github.com/freetushkan/P2Play/actions/runs/45960270448)
и [Silo plugin workflow run
45960270383](https://github.com/freetushkan/P2Play/actions/runs/45960270383)
завершились успешно. Платформенные jobs включали Linux core/API/MediaMesh,
Android, Switch, PS4, PS5 и PS5 package.

Успех этих jobs не означает прохождение browser/Playwright режима
`tools/ui-test.py --browser` или приёмки Android WebView/реальных устройств:
эти проверки остаются отдельными. Статус отражает CI и код на момент merge,
а не универсальную device acceptance.

Публичные compatibility API и wire-format MediaMesh этим шагом не меняются.
