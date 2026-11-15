# SQLite amalgamation

`sqlite3.c` and `sqlite3.h` are the official SQLite amalgamation, **unmodified**.

| | |
|---|---|
| Version | 3.51.3 |
| `SQLITE_SOURCE_ID` | `2026-03-13 10:38:09 737ae4a34738ffa0c3ff7f9bb18df914dd1cad163f28fd6b6e114a344fe6d618` |
| Source | https://www.sqlite.org/2026/sqlite-amalgamation-3510300.zip |
| Zip SHA3-256 (as published on sqlite.org) | `ced02ff9738970f338c9c8e269897b554bcda73f6cf1029d49459e1324dbeaea` |
| `sqlite3.c` SHA-256 | `9512509b1bccb7461f79bea8aad6280ae4699e925fa4804381b71f59e7efb0c5` |
| `sqlite3.h` SHA-256 | `19585c8b5230e9d4f223bf31b709ece7b6a0bb3faf00d8310625d8e58cda1b1d` |

The zip's `shell.c` and `sqlite3ext.h` are not used and not vendored.

`scripts/build.ps1` checks both SHA-256 values before it compiles anything and
compiles `sqlite3.c` once per C runtime with the options in its
`$SqliteDefines` list. `tests/SqliteBuildTests.cpp` checks the version and
those options. `.gitattributes` marks both files `-text`, so git stores and
checks them out byte for byte.

## Updating

1. Download the new amalgamation zip from https://www.sqlite.org/download.html
   and check its SHA3-256 against the value published on that page.
2. Replace `sqlite3.c` and `sqlite3.h` with the zip's copies, unmodified.
3. Update `$SqliteSha256` in `scripts/build.ps1`, the expected version and
   source id in `tests/SqliteBuildTests.cpp`, and the table above.
4. Run `pwsh -File scripts/build.ps1 -Config Release -Test`.

## License

SQLite is in the public domain; see https://www.sqlite.org/copyright.html.
