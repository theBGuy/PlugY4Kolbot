# Live shared stash fixtures

Hand-built files for tests/SharedStashFormatTests.cpp (T-F2). Nothing here comes from a real save or from
BuildClassicStash: synthetic.sss was written byte by byte from the layout below.

## synthetic.sss

171 bytes, SHA-256 `bf0a764678836bed07f2088ee11304d67b728684ee4918a011708e0c147c4ee1`. Little-endian.

| Offset | Bytes | Field |
|---|---|---|
| 0 | `53 53 53 00` | tag "SSS\0" |
| 4 | `30 32` | version "02" (gold follows) |
| 6 | `40 E2 01 00` | gold 123456 |
| 10 | `03 00 00 00` | page count 3 |
| 14 | `53 54` | page 0: "ST" |
| 16 | `01 00 00 00` | page 0: flags 0x00000001 |
| 20 | `00` | page 0: empty name, its NUL |
| 21 | `4A 4D` | page 0: "JM" |
| 23 | `02 00` | page 0: root count 2 |
| 25 | 14 bytes | page 0, item 1: `4A 4D ...` |
| 39 | 27 bytes | page 0, item 2: `4A 4D ...` |
| 66 | `53 54` | page 1: "ST" |
| 68 | `00 00 00 00` | page 1: flags 0 |
| 72 | `52 69 6E 67 73 00` | page 1: name "Rings", its NUL |
| 78 | `4A 4D` | page 1: "JM" |
| 80 | `00 00` | page 1: root count 0 |
| 82 | `53 54` | page 2: "ST" |
| 84 | `03 00 00 00` | page 2: flags 0x00000003 |
| 88 | 20 bytes + `00` | page 2: name "ABCDEFGHIJKLMNOPQRST" (20 bytes, the longest), its NUL |
| 109 | `4A 4D` | page 2: "JM" |
| 111 | `03 00` | page 2: root count 3 |
| 113 | 18 bytes | page 2, item 1: `4A 4D ...` |
| 131 | 31 bytes | page 2, item 2: `4A 4D ...`, with `53 54 4A 4D 00` at its bytes 10..14 |
| 162 | 9 bytes | page 2, item 3: `4A 4D ...` |

Item bytes after the leading `4A 4D` are filler (odd values below 0x80), except the five bytes noted above. The
middle of page 2's item 2 looks like a page tag, an item tag and a NUL, so a split that scans for tags instead of
taking the recorded sizes fails on it.

Flag rule, so that master's loader reads every page WITH its flags: every page's 4 flag bytes contain a 0x00, and
the two bytes after the first 0x00 at or after the flags offset are not `4A 4D`. The loader (src/plugy/InfinityStash.cpp,
loadStash) searches from the flags offset for the first 0x00 and takes the old flag-less form whenever "JM" follows
it, so a page with four non-zero flag bytes, or a first zero flag byte followed by "JM" out of later flag bytes or
the name's first bytes, would be read without flags. BuildClassicStash writes such pages exactly as master's
saveStash does: the ambiguity is a loader quirk that exists today, which this fixture avoids and the builder neither
adds nor refuses. T-F2 asserts the rule over every page of this file.

## synthetic.sizes.txt

The recorded item sizes: one line per page in file order, decimal sizes separated by single spaces, an empty line for
a page with no item. The test tolerates a CR before each LF.

```
14 27

18 31 9
```
