# Changelog fragments

Instead of editing `## [Unreleased]` in [`../CHANGELOG.md`](../CHANGELOG.md)
directly, where every branch collides, each user-facing change adds its **own
file** here. One file per change means parallel branches never conflict on the
changelog. At a stable release, `node scripts/changelog-release.mjs <version>`
assembles these fragments into `CHANGELOG.md`, in the exact
[Keep a Changelog](https://keepachangelog.com/) format, and deletes them. A
pre-release (`-alpha.N`, `-beta.N`, `-rc.N`) leaves them in place: its release
notes, and the `CHANGELOG.md` in its zip and installer, are rendered from them,
and the stable release consumes them later.

**Player-facing changes only.** The release notes on GitHub come from this
changelog, so write for players and bot operators. Build scripts, CI, and repo
tooling get **no fragment**.

CI keys on paths, not user impact: the PR check fires on any change under
`src/`, `game-files/` or `installer/`, so a change there that genuinely needs no
fragment (an internal refactor, say) takes the `no-changelog` label or
`skip-changelog` in the PR title.

## Format

Create a file named **`<category>-<slug>.md`**, where:

- `<category>` is one of `added`, `changed`, or `fixed` (the Keep a Changelog
  groups this project uses: `feat` → `added`, `fix` → `fixed`, refactors/perf →
  `changed`). A pending `added-*` fragment makes the release driver suggest a
  minor version.
- `<slug>` is a short kebab-case topic. Keep it distinctive; append the PR number
  if you want to be certain it's unique (e.g. `added-shared-stash-locking-42.md`).

The **file body is the finished changelog bullet, verbatim**, including the
leading `- ` and the 2-space indent on wrapped continuation lines. Write it *for
humans*: a clear sentence about what changed for the player, not your commit
subject. State what the change gives the reader, not the defect it removes, and
open with a short **bold lead** naming it, so the release notes can be skimmed:
`- **Safer stash saves.** PlugY writes ...`. The assembler concatenates bodies
under the right `###` heading without re-wrapping, so what you write is exactly
what `CHANGELOG.md` gets. GitHub breaks a release body at every newline, so the
release body joins each wrapped line back onto its bullet, as long as the line
holds plain text. What it leaves as written:

- A wrapped line starting with `>`, `# ` or a list marker, or the line after a
  `code span` that crosses a line break: that line and the rest of its bullet.
- A wrapped line starting with `<` or `|`, or a link definition: that line and
  every later line up to the next blank one, so the rest of its `###` group.
- A code fence, a `$$` line, a raw HTML block, or a `<pre>`, `<code>` or
  `<textarea>` tag still open at the end of its group (even inside backticks):
  everything from there to the end of the release notes.

So rewrap to move such a word off the start of a line, keep each code span on
one line, and keep those three tag names out of fragments.

Use at most one em dash per bullet; two read as a paired aside and fail
`--check`. Em dashes inside backticks, **bold**, or "quotes" don't count, so
quoted game text can keep its own.

A fragment counts as new in the next pre-release's notes when its file name was
not in the previous pre-release. Rewording a fragment keeps its name, so it stays
out of the new entries; to put a substantially changed entry back on top, rename
its fragment.

### Example: `added-show-error-message-box.md`

```md
- **`ShowErrorMessageBox` option** in the `[GENERAL]` section of `PlugY.ini`, for
  unattended bots. Set it to 0 and the errors `PlugY.dll` raises in the game are
  reported without a message box waiting for a click.
```

### Example: `fixed-autosave-stash-files.md`

```md
- **Stashes save with the character.** In single-player games on Diablo II
  1.14d, the PlugY stash (`.d2x`) and the shared stash are saved with the
  character every time the game saves it.
```

## Commands

- `node scripts/changelog-draft.mjs --preview` shows the pending fragments
  assembled under an Unreleased heading (nothing is written).
- `node scripts/changelog-draft.mjs --check` validates every pending fragment
  (filename + bullet format) and reports all problems; the PR workflow runs
  this, so a malformed fragment fails the PR instead of the release.
- `node scripts/changelog-draft.mjs` drafts starting-point bullets from the git
  history since the last stable release tag (curate before saving as a
  fragment, opening it with a bold lead in place of any drafted `**scope:**`
  prefix).
- `node scripts/release.mjs` (maintainers) is the interactive release driver:
  it checks the repo state, previews the fragments, picks the version (patch,
  minor, major or a pre-release), runs `changelog-release.mjs`, then commits,
  creates the annotated tag, and pushes master and the tag together. Rehearse
  with `--dry-run`; `--help` lists the options.
- `node scripts/changelog-release.mjs <version>` (maintainers) stamps the version
  into `src/common/PlugYVersion.h` and the version banners of the INIs,
  `LocalizedStrings.ini` and readmes in `game-files/`; for a stable version it
  also assembles the fragments into `CHANGELOG.md` as `## [<version>] - <date>`
  and deletes them.
- `node scripts/changelog-extract.mjs <tag>` prints the release notes for a tag
  (the release workflow uses it for the GitHub release body), with wrapped
  paragraph and bullet lines joined. A pre-release's notes, like its packaged
  `CHANGELOG.md`, open with a paragraph that names the newest stable version in
  `CHANGELOG.md` (or "the last stable release" when it has none) and says that
  the pre-release's installer turns on full session logs and the pre-release
  update channel (`PRERELEASE_SETTINGS` in `scripts/changelog-lib.mjs`). The
  previous pre-release is the highest pre-release tag (`v`, then a version with
  `-alpha.N`, `-beta.N` or `-rc.N`) below the version being released and above
  that stable version. When there is one, the notes lead with the fragments whose
  file names its tagged `changelog.d/` lacked, under "New since <version>", and
  list every pending entry below them in a collapsed block. Without one, when git
  can't read it (the script says so on stderr), or when every entry is new, the
  notes are the whole list alone, and when no entry is new, their opening
  paragraph says so.
- `node scripts/changelog-package.mjs <pre-release version> <output file>`
  writes the `CHANGELOG.md` a pre-release's zip and installer ship: the
  repository's file plus a section for that version built from the pending
  fragments (`scripts/package.ps1` runs it; nothing in the tree changes).

`README.md` in this directory is ignored by the tooling and never consumed.
