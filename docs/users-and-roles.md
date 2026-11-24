# User roles and permissions

P2Play follows TorrServer.Silo numeric rank. UI uses named presets; stored
`rank` remains Silo-compatible.

| Role | Rank | Notes |
|---|---:|---|
| Owner | 100 | Fixed ID `owner`; cannot be deleted/demoted |
| Administrator | 50–99 | Manages lower-ranked accounts |
| User | 10–49 | New users default to rank 10 |
| Guest | 1–9 | Lowest access |

Enforcement exists in `src/core/auth.cpp` and `src/core/api_silo.cpp`.
Only a strictly higher rank can create/promote/manage a target; passwords can
be changed for self or lower-ranked users; owner cannot be removed/demoted.
API: GET/POST /api/users, PUT /api/users/:id, PUT /api/users/:id/rank,
DELETE /api/users/:id. UI must not replace server-side checks.

## pPlay settings UI

Under each P2Play network: separated Users block, dynamic account rows, final
separated **Add user** row. User submenu: change password, role
(Administrator/User/Guest), delete with controller-compatible confirmation.
Owner is visible but immutable. No token reset in pPlay; reserve it for future
web UI. Reuse this UI for P2Play_embedded.
