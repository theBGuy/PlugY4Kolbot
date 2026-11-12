# Stash protocol for scripts (Diablo II 1.14d)

A bot or script can drive PlugY's multi-page stash without ever leaving its home page. It can show any page,
list what every page holds, and move a stash item to any page by its gid, with one request each and an answer
it can wait for. Normal page moves keep the page shown in D2BS's view. A shared database item changed by
another game may get a new local gid, so use LIST again after a stale or removed item.

kolbot's own client for this protocol ships separately from PlugY4Kolbot. This page is the wire reference
for anyone writing a client.

## Requirements

- Diablo II 1.14d, single player or a TCP/IP game hosted by PlugY4Kolbot. Never on realms.
- `[STASH] ActiveMultiPageStash=1` and `ActiveStashProtocol=1` (the default) in PlugY.ini.
- D2BS with `EnableUnsupported=true`, so scripts can call `sendPacket`.
- A TCP/IP joiner must run PlugY4Kolbot with the same stash grid as the host. Otherwise the host refuses its
  requests with `CLIENT_MISMATCH`, rather than letting its view of the stash go stale.
- Live SQLite shared-stash access is for single-player games on one PC, using one local save folder and
  the same grid. A migrated shared stash is refused by the legacy TCP/IP path. See
  [shared stash transactions](shared-stash-transactions.md) before first use.

## 1. Handshake

Send the 3-byte packet `3A 3A seq` (`US_PY_HELLO`, `seq` any byte you choose). The server answers with a
`UC_PROTO` message, then a `UC_ACK` whose op is `0x80`.

Before sending any request:
- The ACK's feature bit `0x10` must be set. Without it, never send a `0x55` packet: a host without
  PlugY4Kolbot stalls on it. Upstream PlugY or a vanilla host doesn't answer HELLO at all; a short timeout is
  the answer.
- The ACK's `clientProto` must be 1 or more, and `clientGridMatch` 1. The client PlugY confirms the protocol
  when it receives the join-time `UC_PROTO`, so a HELLO sent right after joining can come back with
  `clientProto` 0. Wait a moment and send HELLO again (each HELLO re-sends `UC_PROTO`).

The public protocol version and the 16-byte request remain version 1. `UC_PROTO`'s 32-bit flags at
offset `+0C` announce database transactions with bit `0x02` (`PY_PROTO_SHARED_TRANSACTIONS`). This is
separate from the ACK's one-byte feature bits. The client PlugY adds revision and request-ID checks
for shared actions; scripts still send their normal `0x55` requests.

## 2. Requests: packet `0x55`, exactly 16 bytes, little-endian

| Byte | Field |
|---|---|
| 0 | `0x55` |
| 1 | op |
| 2 | seq (echoed in the answer) |
| 3 | flags |
| 4-7 | item gid (D2BS `item.gid`) for MOVE, else 0 |
| 8-11 | page id (0 = the first page). LIST: the first page listed |
| 12 | x (MOVE without `AUTO`) |
| 13 | y |
| 14-15 | LIST: most pages listed (0 = no limit), else 0 |

| Op | Name | What it does |
|---|---|---|
| `0x01` | MOVE | Moves stash item *gid* to page *id* (or the page shown with `CURRENT`) at x,y (or the first free spot with `AUTO`). The page shown doesn't change. |
| `0x02` | SELECT | Shows page *id*, creating missing pages up to it. With `CURRENT` it only reports the page shown. |
| `0x03` | LIST | Sends a `UC_STASH_PAGE` message per page (more than one for a page with over 26 items), then the ACK. |

| Flag | Name | Meaning |
|---|---|---|
| `0x01` | SHARED | The page is on the shared list (SELECT, MOVE target, LIST). |
| `0x02` | AUTO | MOVE: the first free spot, column by column from (0,0); x and y are ignored. |
| `0x04` | ALLOW_PINNED | MOVE: a pinned item (see `PinnedItems`) may move. |
| `0x08` | CURRENT | The page shown (page id and SHARED ignored). |
| `0x10` | CROSS | MOVE: the item may change list (personal <-> shared). |
| `0x20` | WITH_EMPTY | LIST: also list the empty pages after the last page with items. |
| `0x40` | ALLOW_SHARED | MOVE: a shared page may be the source or the target. |
| `0x80` | reserved | Must be 0. |

A flag an op doesn't use is refused with `BAD_REQUEST`.

## 3. Answers: `0x9D` messages

Each `0x9D` message's size is its byte +2. Parse by that, never by the length of the event array: one event can
hold several messages. `UC_ACK`, `UC_ITEM_MOVED`, and `UC_STASH_PAGE` keep bytes +04..+07 at 0;
the private database messages below use those bytes for the player gid.

### `UC_ACK` (+01 = `0x21`, 36 bytes): one per request

| Offset | Field |
|---|---|
| +03 | seq |
| +08 | op (`0x80` for HELLO) |
| +09 | status (table below) |
| +0A | protocol version (1) |
| +0B | features (table below) |
| +0C, +0D | server stash grid width, height |
| +0E | the version this client's PlugY confirmed (0 = none yet) |
| +0F | 1 when the client's grid matches the server's |
| +10 (4) | MOVE: the item gid; LIST: the number of pages sent |
| +14, +15 | MOVE: the item's final x, y |
| +18 (4) | MOVE: the item's final page id; others: the page shown |
| +1C (4) | page flags: `0x01` shared, `0x02` index, `0x04` main index, `0x100` the page shown, `0x200` SELECT changed nothing |
| +20, +22 (2 each) | number of personal pages, shared pages |

Match an ACK by op and seq, and for MOVE also by gid. A request gets no ACK when the player is dead (the game
drops it) or has no PlugY data, so always use a timeout.

| Status | Name | What to do |
|---|---|---|
| 0 | OK | Done. |
| 1 | NOOP | Already there. A MOVE also re-sends where the item is (see repair, below). |
| 2 | NOT_FOUND | No item with that gid. |
| 3 | NOT_IN_STASH | The item is in the inventory, belt, cube or equipped. MOVE only takes stash items: put it on the page shown with a normal move first. |
| 4 | PINNED | Pinned item; set ALLOW_PINNED to move it anyway. |
| 5 | PAGE_RANGE | Beyond `MaxPersonnalPages` / `MaxSharedPages`. Nothing was created. |
| 6 | NO_ROOM | No room at x,y, or no free spot with AUTO. Nothing changed and no page was created. |
| 7 | FEATURE_OFF | Multi-page stash off, shared stash off or not loaded, or a shared page without ALLOW_SHARED. |
| 8 | BUSY | The game holds the item for a moment; retry after about 100 ms. |
| 9 | BAD_REQUEST | Unknown op or flag. |
| 10 | CROSS_LIST | A personal <-> shared move without CROSS. |
| 11 | INTERNAL | Should not happen. The item stays where it was or where a page switch would put it; the session log in PlugY\logs says why. |
| 12 | CLIENT_MISMATCH | The client hasn't confirmed the protocol, or its grid differs. Send HELLO again, then retry. |
| 13 | SHARED_CONFLICT | Legacy mode only: shared moves are refused because the shared stash is in conflict. A running game doesn't send it: PlugY reports a conflict only in the save made when the player leaves. |
| 14 | STALE | The shared view changed. LIST again, find the current local gid and page, then make a new request. |
| 15 | STORE_ERROR | The shared database could not accept this new action. Check the session log and storage before retrying. An earlier pending action may still finish; this is not a rollback notice. Never fall back to old `.sss` items. |

| Feature bit | Meaning |
|---|---|
| `0x01` | Multi-page stash on, with pages for this character (SELECT can work) |
| `0x02` | Shared stash on |
| `0x04` | This game loaded the shared stash |
| `0x08` | This game loaded the personal stash |
| `0x10` | Packet `0x55` is installed: requests may be sent |
| `0x20` | Swaps keep pinned items on their page |
| `0x40` | Legacy shared-file conflict; database mode uses live `BUSY`, `STALE`, and `STORE_ERROR` results instead |
| `0x80` | Page insert/delete keeps this client's pages in step |

### `UC_ITEM_MOVED` (+01 = `0x20`, 36 bytes)

Sent before a MOVE's ACK, and on its own (seq 0) when a page switch had to put an item somewhere else. The
client's PlugY applies it to its own copy of the item, so a script only needs it to know what happened.

| Offset | Field |
|---|---|
| +03 | the request's seq, 0 when unsolicited |
| +08 | source flags: `0x01` shared, `0x02` it left the page shown, `0x04` source unknown |
| +09 | target flags: `0x01` shared |
| +0A | reason: 0 a MOVE, 1 repair (the item was already there), 2 relocated by a page switch |
| +10 (4) | gid |
| +14, +15 | x, y |
| +18 (4) | target page id |
| +1C (4) | source page id (`0xFFFFFFFF` = unknown) |

### `UC_STASH_PAGE` (+01 = `0x22`, `0x28 + 8 * n` bytes, at most 248)

| Offset | Field |
|---|---|
| +03 | seq |
| +08 (4) | page id |
| +0C (4) | page flags (as in the ACK) |
| +10, +11 | chunk index, chunk count (1 for an empty page) |
| +12 | n, the items in this chunk (at most 26) |
| +14 (20) | page name as saved, NUL padded (no NUL when it is 20 characters long), empty when unnamed; a page PlugY adds for items that no longer fit is `Moved items` in every game language |
| +28 | n records of 8 bytes: gid (4), classid (2), x, y |

Plain LIST sends every page from the first one asked for up to the last page with items, empty pages in
between included. `WITH_EMPTY` adds the empty pages after it.

### `UC_PROTO` (+01 = `0x23`) and `UC_PAGE_OP` (+01 = `0x24`)

Both are for the client's own PlugY. `UC_PROTO` carries the version, the stash grid and the pinned item codes;
`UC_PAGE_OP` tells the client a page was inserted or deleted. A script can ignore both.

### Private database messages (`0x25` through `0x28`)

The client PlugY applies these 36-byte messages. Scripts should leave them in the receive buffer and
ignore them unless they implement the database client itself.

| Function | Purpose |
|---|---|
| `0x25` | Commit revision and session epoch, sent after the matching item/page updates. |
| `0x26` | Detach a hidden shared item before its native unit-removal message. |
| `0x27` | Reset shared-page layout while keeping native item units and gids alive for the following moves. |
| `0x28` | Close the shared cube view when its ownership changes in another game. |

Each carries the player gid at `+04`. Revision messages carry a 64-bit revision at `+08` and a 64-bit
session epoch at `+10` (offsets are hexadecimal). These messages are distinct from native `0x9C`/`0x9D` item updates.

The client send hook also puts a private 26-byte packet `0x56` immediately before a guarded action:
revision (8 bytes at `+01`), request counter (8 at `+09`), session epoch (8 at `+11`), and the following
action's packet ID (`+19`). Offsets here are hexadecimal. This packet is generated by PlugY; scripts
must not make their own stamps or reuse another game's revision, counter, epoch, or gids.

## 4. Rules that keep items safe

- Register the `gamepacket` listener only while a request waits, and always return `false` from it: returning
  `true` drops the rest of that receive buffer, PlugY's messages included.
- MOVE is idempotent while that local item still exists. If its answer is lost, send the same MOVE again:
  it answers NOOP and re-sends where the item is (`UC_ITEM_MOVED` reason 1). On `STALE` or `NOT_FOUND`,
  LIST again before choosing another request; never substitute a gid from another game.
- Items on hidden pages are out of `me.getItems()`, but `getUnit(4, -1, -1, gid)` still finds each one. Use
  LIST to know where they are.
- Insert and delete page requests (`/insertpage`, `/deletepage`, or the raw `0x3A` packets) are run by the
  server, which then tells the client, so the pages a script sees always match the pages saved.
- With `PinnedItems` (`box` by default), swapping pages leaves pinned items where they are on both pages, and a
  swap that would put a pinned item and another item on the same cells is refused.
- In database mode, a successful shared action commits before its success updates are sent. A failed commit
  keeps ownership reserved and retries the commit without repeating the native action. Character saves wait
  while that result is pending. `BUSY` is a reason to wait; `STALE` is a reason to LIST again.
- The old save-time conflict-file rule applies only to the legacy shared-stash mode. Database mode never
  writes a whole local shared-stash copy over the database at exit. Shared MOVE flags remain explicit.
