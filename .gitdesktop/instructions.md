# Commit Messages & PR Titles: Conventional Commits

All commit messages and PR titles must follow **Conventional Commits** format.

## Format

```
<type>(<scope>): <description>
```

- **type**: One of `feat`, `fix`, `perf`, `refactor`, `revert`, `docs`, `test`, `build`, `ci`, `chore`, `style`
- **scope**: One or more hyphenated feature areas (comma-separated if multiple), e.g. `stash`, `saves`,
  `tcpip`, `installer`, `updater`, `patcher`, `launcher`, `logs`, `ini`, `release`
- **description**: Short, imperative message (no period at the end)

`build` covers the build, packaging and release scripts (`scripts/`); `ci` covers `.github/`.

## Examples

✅ **Good**
- `feat(stash): keep pinned items on their page`
- `fix(tcpip): validate save transfer chunks`
- `feat(installer,patcher): offer to patch Game.exe`
- `docs(readme): document the update channels`
- `build(release): ship each pre-release's changes in its packaged CHANGELOG.md`

❌ **Avoid**
- `add pinned items` (missing type and scope)
- `feat(stash-protocol)` (missing description)
- `Fix: the stash thing` (capitalized, vague scope)
- `fix: various installer fixes` (missing scope, vague description)
- `feat(stash,docs): show page names` (`docs` is a type, not a feature area)

## Tips

- **Scope should name the feature area**, not the file or function (use `stash`, not `InfinityStash`); docs that
  ship with a feature take the feature's scope
- **Multiple scopes** for cross-cutting changes: `fix(saves,tcpip): keep the joiner's stash files`
- **Type guides later tooling** — `node scripts/changelog-draft.mjs` drafts starting bullets from the `feat`
  (Added), `fix` (Fixed) and `perf`/`refactor`/`revert` (Changed) subjects since the last stable tag, and skips the
  other types. The release notes still come only from `changelog.d/` fragments, so a commit type never replaces one
- Start the description **lowercase** (names such as `Game.exe` or TCP/IP keep their case) and use **no period**
- **Branch names take the same types** as a `<type>/` prefix: `feat/pinned-items`, `fix/tcpip-save-transfer`,
  `build/prerelease-changelog` — never `feature/`

# PR descriptions

Reviewers, the review bot and later readers take a PR description as a statement of what the code does, so
every sentence must hold against the diff.

- Describe a file only from what its diff changes; never infer its role from its name. `UpdateClient.cpp`
  and `UpdateServer.cpp` are PlugY's client/server packet handlers, not the update check. When the diff was
  cut short, name the files you could not see instead of guessing what they do.
- Keep every condition the code has: the game version (many PlugY4Kolbot features work on 1.14d only), the
  INI setting that turns a feature on, single player vs TCP/IP, new installs vs upgrades.
- When a change is deliberately narrow, say what stays the same ("the saved page name stays English; only
  the stash shows the translation"). Never extend a change to code its diff doesn't touch.
- Describe tests by what their test cases check, never by the topics the feature covers.
- Lead with what changes for players and bot operators. When nothing does (build, CI, tests, repo tooling),
  say so and lead with what changes for maintainers; never invent player impact. Every behavior change gets a
  line, even a one-line diff (a new default, a fix in a shared loader). File paths support a claim; they are not
  the claim.
- Use exact verbs: "moves" or "renames" for moved code, "rewrites" for replaced code, "adds" only for new
  files or features.
- A list introduced with "including" names every item, or says "for example".

# Documentation: five surfaces, one change

A **user-facing** change updates its documentation in the SAME change — never in a
follow-up. This project keeps five documentation surfaces, and they are ONE concern:
a change that leaves any of them stale is one gap with several locations, not one
gap per file.

1. **`README.md`** — the *FEATURES* list, plus *Installing*, *Updating*, *Privacy* or
   *Logs* when the change touches them.
2. **Game-file readmes** — `game-files/PlugY_The_Survival_Kit_-_Readme.txt` (English):
   its FEATURES, CONTENTS (file list and count), INSTALLATION, UPDATING, CONFIGURATION
   and COMMENTS ON THE CONFIGURATION FILE sections (one line per INI key with its
   values). The German (`Liesmich`), French (`LisezMoi`) and Traditional Chinese
   (`讀我檔案`) readmes lag the English one; a change that doesn't update them says so
   in the PR. A change that brings the Traditional Chinese readme up to date also runs
   `node scripts/zh-readme-mark.mjs`, which updates `scripts/zh-readme-translated-to.txt`
   for the check `release.mjs` makes before a release.
3. **Shipped INI files** — a new or changed setting goes into `game-files/PlugY.ini`
   and `game-files/PlugY/PlugYDefault.ini` with the same default, plus its line in the
   English readme.
4. **`docs/stash-protocol.md`** — the contract kolbot's PlugY module is written
   against. Any change to the stash protocol (packets, the D2BS API, result codes)
   updates it.
5. **Changelog fragment** — `changelog.d/<added|changed|fixed>-<slug>.md`, whose body
   is the finished Keep a Changelog bullet, written for players and bot operators:
   state what the change gives them, not the defect it removes. Never edit
   `## [Unreleased]` in `CHANGELOG.md` directly; fragments are assembled there at
   release time. The `fragment` check is required on master and keys on paths: any
   `src/`, `game-files/`, `installer/` or `third_party/` change needs a fragment, the
   `no-changelog` label, or `skip-changelog` in the PR title. Build scripts, CI and
   repo tooling get no fragment.

Everything under `game-files/` ships byte-exact (`-text`, CRLF; PlugY's INI parser
needs CRLF) in several encodings: the English, German and French readmes are UTF-8,
the Traditional Chinese readme and `PlugY/LocalizedStrings.ini` are UTF-16LE with a
BOM, and `PlugY.ini` and `PlugY/PlugYDefault.ini` are Windows-1252. Edit them with
encoding-aware tools and keep each file's encoding and line endings.

When a change alters **existing** behavior, grep all five surfaces for the old
wording rather than updating the spots you remember — stale copies of the same
claim hide across surfaces, and a byte grep can't see into the UTF-16 files, so
decode those first. A change too minor for the READMEs may carry only the
changelog fragment, but that is a deliberate call to state, not a step to skip
silently. A change with no user-facing effect (pure refactor, internal rename,
test-only, build or CI) needs none of them.
