# Shared stash transactions

On Diablo II 1.14d, single-player games on one PC use a SQLite database for the
shared stash. This is the default when the shared multi-page stash is on. Mouse
actions and bot requests go through the same ownership checks. Each game keeps a
local view; another game's changes arrive without a rejoin.

The existing `ActiveMultiPageStash=1` and `ActiveSharedStash=1` settings enable it;
there is no separate database-mode switch or service to start.

This mode needs a local save folder, the same stash grid in every game, and the
current PlugY4Kolbot build and matching item dimensions in every install using
that folder. Database file links and network paths are refused. It does not serve
several PCs or TCP/IP shared-stash transfers. This mode currently refuses games
with custom DLLs loaded because their extra shared-save data cannot be imported
safely. It supports up to 65,535 shared pages.

When a shared cube is taken out, its contents go with it. Empty a character's
private cube first if its contents would conflict with that transfer. Legacy
character-owned cube contents without a personal cube are kept on personal
`Moved items` pages, rather than silently added to a shared cube.

## First use and backups

Close all games that use the stash before the first start with this build.
PlugY imports the existing `.sss` once. It keeps page order, names, flags, gold,
and native item data, including sockets. `SharedStashFilename` and the existing
hardcore/softcore split still choose which stash is used.

For a stash named `_LOD_SharedStashSave`, the files are:

| File | Purpose |
|---|---|
| `_LOD_SharedStashSave.sqlite3` | Current shared stash |
| `_LOD_SharedStashSave.sqlite3-wal` and `-shm` | SQLite's live write and shared-memory files |
| `_LOD_SharedStashSave.sqlite3.pre-sqlite.sss` | Exact original import bytes; an empty seed on first use with no old stash |
| `_LOD_SharedStashSave.sqlite3.backup.sqlite3` | Consistent SQLite backup made at session start |
| `_LOD_SharedStashSave.sss` | Migration marker that makes old PlugY builds refuse this stash |

An interrupted import can resume from its protected seed. Once a database exists,
an empty, damaged, locked, or unreadable database never causes a fallback to old
`.sss` items. Do not delete the database or the marker to force such a fallback.
A migrated stash cannot be used by older PlugY builds or by the legacy TCP/IP
shared-stash path.

Close every game before making an ordinary file copy of a database for backup.
The in-game backup uses SQLite's backup API, so it includes committed WAL data;
copying just a live `.sqlite3` file is not a complete backup.

## How an action commits

The database holds stable item and page IDs, item versions, gold, occupied cells,
and pending operations. Game `gid` values are local to each game. A large item
owns every cell under its full width and height. Cube contents have their own
3x4 cell grid and belong to their cube; socketed items stay in the native item
bytes.

1. The server checks the request's view and reserves the affected ownership.
   Competing actions on the same item or cells cannot both take them.
2. The native game action runs once. Its item updates and success messages wait
   while the resulting shared state is captured.
3. SQLite commits the result as one transaction, releases the reservation, and
   the game sends the retained updates. Other games then refresh their views.

The guarded paths include item pickup and placement, swaps, stacking, use,
sockets, cube contents and transmute, page changes, shared gold, and script moves.
Page refresh also creates and removes native units. Unchanged shared units keep
their local gids; scripts must list again after a changed or removed item.

A busy or stale action is refused before the native action runs. A refused mouse
move restores the server's cursor state. Scripts receive `BUSY`, `STALE`, or
`STORE_ERROR`; see the [stash protocol](stash-protocol.md).

If a database write fails after the native action, ownership stays reserved and
client updates stay held. The server retries the commit; it does not run the
action again. Character saves wait until finalization is complete. Reservations
have no timer that can expire while their owner still runs. Recovery checks the
owning Windows process and its creation time before releasing a dead owner's
reservation.

SQLite uses WAL and full synchronization. Reads can run together; its short write
transactions run in order. No separate service is needed.

## Character saves and limits

The shared database commits during play. Autosave and exit do not write a whole
local shared-stash copy over it. Personal stash and character files keep their
existing save path. Shared roots are kept out of the character serializer. While
a shared cube is open, saving temporarily uses the character's private cube
contents, then restores the live shared view. Native units and their gids stay
the same through that temporary save view.

Database mode keeps the save-serialization hook even with
`ActiveCrashSafeSaves=0`, so its ownership checks and cube save handling remain
active. That option cannot bypass a pending transaction.

Crash atomicity across SQLite and the character's `.d2s`/`.d2x` files is outside
this change. A kill or power loss between those separate stores can still cause
item loss or copies across them. The database's own transaction recovery does not
make those files one transaction.

## Verification state

The Release build and native test suites pass without warnings. Storage tests
include real eight-process races; isolated two-game checks cover mouse deposit,
remote refresh, independent writes, competing pickup and placement, cube
transmute and transfer, sockets, stacking, item use, shared gold, pinned page
swaps, legacy import and rejection of unsupported trailers.

An injected write failure kept ownership and client updates held until the same
operation committed once. Save tests checked native character item counts
against private inventories, then reloaded shared roots, full cube contents,
and socket data. Unchanged unit gids survived remote page changes. Same-frame
private packet batches and normal game exit were also checked.

The final build hashes, source/object timestamps, per-case client/server views,
and database snapshots are recorded in the local harness report and
`harness/live/final-build-record.json`. The harness uses its own game copies and
save folders; its fixture and packet controls exist only in `PLUGY_TEST_HOOKS`
builds.

The native bridge was checked against the installed 1.14d executable, file version
`1.14.3.71`, SHA-256
`631066c1649c4ea9ffe48bf97e24c00bca1f7a6759c21150f1a79982589adaaf`.
Offline IDA evidence and isolated runtime artifacts are under
`build/native-stash-analysis/` and `harness/live/`. Neither the original game
installation nor the user's original saves were changed for these tests.
