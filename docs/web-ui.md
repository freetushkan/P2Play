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
| GET /api/v1/torrents/{hash} | Запись из собственной библиотеки |
| PUT /api/v1/torrents/{hash} | Изменение title и/или category |
| DELETE /api/v1/torrents/{hash} | Удаление из библиотеки текущего пользователя |

POST добавляет запись в библиотеку по умолчанию. JSON-поля: link, title,
category. Для multipart используются поля file, title, category; файл и ссылка
одновременно недопустимы, пустой файл отклоняется. Upload limit нативного
маршрута ограничен HTTP body limit core: 16 MiB вместе с multipart framing;
файл должен быть немного меньше из-за multipart boundary и полей.

Поиск разбивает запрос на слова, требует совпадения каждого слова в title,
infohash или category и использует общую Unicode-fold функцию MediaMesh
(включая кириллицу и ё/е). Категории: все (category не задана), без категории
(category=none) или точное значение без учёта регистра. Фильтрация и сортировка
производятся по всей библиотеке до offset/limit. Сортировка детерминирована
infohash как tie-breaker.

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
- Изменение категории и подтверждаемое удаление с ясной пометкой про общие
  torrent metadata и per-user library membership.
- Ошибка не очищает форму; повторный submit блокируется на время операции.
- Недоверенные строки выводятся через textContent, внешние постеры/HTML не
  вставляются, CSP остаётся ограничивающей.

## Проверки и дальнейший паритет

tools/ui-test.py проверяет защищённый API, добавление magnet и multipart
torrent, поиск/категории/сортировку, удаление и изоляцию двух пользовательских
библиотек. Browser режим проверяет login, вкладку Library и основные действия
через Playwright.

Это не означает полный TorrServer parity. Остальные сценарии MatriX/Silo,
файлы/папки, stream/player, viewed/reset/progress, сведения/peers/cache,
настройки и управление users проверяются отдельными шагами. Browser CI на Linux
не заменяет приёмку Android WebView, embedded LAN и целевых устройств.

Публичные compatibility API и wire-format MediaMesh этим шагом не меняются.
