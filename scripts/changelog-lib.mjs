// Shared helpers for the changelog-fragment workflow. Fragments live in
// `changelog.d/` as `<added|changed|fixed>-<slug>.md` files whose body is the
// finished Keep a Changelog bullet; `changelog-release.mjs` assembles them into
// CHANGELOG.md, `changelog-extract.mjs` renders release notes from them,
// `changelog-package.mjs` renders a pre-release's packaged CHANGELOG.md, and
// `changelog-draft.mjs --preview` previews them. Node built-ins only (no deps).
// The repo stores CHANGELOG.md as LF, so every write into the tree goes through
// the LF helpers here. changelog-package.mjs writes only outside the tree and
// keeps the checkout's line endings, like the CHANGELOG.md a stable release ships.
import { readdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { compareVersions, parseVersion } from "./version-lib.mjs";

// The Keep a Changelog groups we use, in the order they render.
export const CATEGORY_ORDER = ["Added", "Changed", "Fixed"];

// A fragment file: `<category>-<slug>.md`. README.md / .gitkeep don't match, so
// they're never consumed. Case-insensitive so `Added-foo.md` still parses.
export const FRAGMENT_RE = /^(added|changed|fixed)-.+\.md$/i;

const emptyGroups = () =>
  Object.fromEntries(CATEGORY_ORDER.map((c) => [c, []]));

const toLF = (text) => text.replace(/\r\n/g, "\n");

/**
 * Read a file and normalise its line endings to LF. A leading UTF-8 BOM is
 * dropped: Windows PowerShell 5.1's `Set-Content -Encoding utf8` writes one,
 * and it would hide the fragment's leading "- " from every check.
 */
export function readLF(path) {
  return toLF(readFileSync(path, "utf8")).replace(/^\uFEFF/, "");
}

/** Today's date as YYYY-MM-DD in local time (the maintainer's calendar day, not UTC's). */
export function localDate(d = new Date()) {
  const pad = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

/** Write a file as LF, whatever the input used. */
export function writeLF(path, text) {
  writeFileSync(path, toLF(text), "utf8");
}

const categoryTitle = (lc) =>
  lc.charAt(0).toUpperCase() + lc.slice(1).toLowerCase();

/**
 * Read every fragment in `dir`, grouped by category. Filenames are sorted with
 * localeCompare so output is deterministic regardless of FS enumeration order.
 * Throws (naming the file) on a malformed fragment. Returns
 * `{ files: string[], groups: { Added, Changed, Fixed }, entries }`; `files` are
 * the absolute paths that were consumed (for deletion by the release script),
 * and `entries` the same fragments in the same order as `{ name, category, body }`.
 */
export function readFragments(dir) {
  let names;
  try {
    names = readdirSync(dir);
  } catch {
    return { files: [], groups: emptyGroups(), entries: [] };
  }
  const groups = emptyGroups();
  const files = [];
  const entries = [];
  for (const name of names
    .filter((n) => FRAGMENT_RE.test(n))
    .sort((a, b) => a.localeCompare(b))) {
    const path = join(dir, name);
    const category = categoryTitle(name.match(FRAGMENT_RE)[1]);
    const body = readLF(path).replace(/\n+$/, "");
    if (!body.startsWith("- ")) {
      throw new Error(
        `Fragment ${name} must start with "- " (a finished Markdown bullet). Got: ${JSON.stringify(
          body.slice(0, 60),
        )}`,
      );
    }
    groups[category].push(body);
    files.push(path);
    entries.push({ name, category, body });
  }
  return { files, groups, entries };
}

/**
 * The fragments `entries` (from readFragments) whose file name is not among
 * `previousNames`, grouped like readFragments' `groups`. Names compare without
 * case. A fragment counts by its name alone: rewording keeps it out, and renaming
 * a substantially changed entry's file is how it comes back as new.
 */
export function newSinceGroups(entries, previousNames) {
  const before = new Set([...previousNames].map((n) => n.toLowerCase()));
  const groups = emptyGroups();
  for (const { name, category, body } of entries) {
    if (!before.has(name.toLowerCase())) groups[category].push(body);
  }
  return groups;
}

/**
 * The pre-release a pre-release's notes count their new entries from: the
 * highest `v`-prefixed pre-release tag in `git tag --list` output below
 * `version` and above `lastStable` (no lower bound when null), without its
 * "v". Any base counts, so 15.0.0-rc.3 precedes 15.1.0-rc.1. Null when none.
 */
export function previousPrerelease(tagListOutput, version, lastStable) {
  if (!parseVersion(version)) return null;
  const floor = parseVersion(lastStable) ? lastStable : null;
  const candidates = (tagListOutput ?? "")
    .split("\n")
    .map((t) => t.trim())
    .filter((t) => t.startsWith("v") && parseVersion(t.slice(1))?.channel)
    .map((t) => t.slice(1))
    .filter(
      (v) => compareVersions(v, version) < 0 && (floor === null || compareVersions(v, floor) > 0),
    );
  candidates.sort((a, b) => compareVersions(b, a));
  return candidates[0] ?? null;
}

// Split a fragment body into its bullets, each flattened to a single line: a
// "- " line opens a bullet, "  "-indented lines continue it, blanks are skipped.
const bulletsOf = (lines) => {
  const bullets = [];
  for (const line of lines) {
    if (line.startsWith("- ")) bullets.push([line.slice(2).trim()]);
    else if (line.trim() === "") continue;
    else if (line.startsWith("  ") && bullets.length > 0)
      bullets[bullets.length - 1].push(line.trim());
  }
  return bullets.map((parts) => parts.join(" "));
};

// Quoted UI copy is the game's own wording and legitimately carries em-dashes,
// so code, bold, and double-quoted spans come out before the count.
const withoutQuotedSpans = (text) =>
  text
    .replace(/`[^`]*`/g, "")
    .replace(/\*\*[^*]+\*\*/g, "")
    .replace(/"[^"]*"/g, "");

/**
 * Validate every changelog.d entry without assembling. Returns a list of
 * `{ file, problem }`, empty when everything is consumable. Unlike
 * readFragments (which throws on the FIRST bad body, at assembly time), this
 * collects every problem so CI and the release driver can report them all at
 * once. It also catches the one failure readFragments can never see: a
 * misnamed `.md` file that FRAGMENT_RE doesn't match (e.g. `add-foo.md`),
 * which the assembler would silently ignore forever.
 */
export function validateFragments(dir) {
  let names;
  try {
    names = readdirSync(dir);
  } catch {
    return [];
  }
  const problems = [];
  for (const name of names.sort((a, b) => a.localeCompare(b))) {
    // Case-insensitive like FRAGMENT_RE, so `added-x.MD` is validated as well as consumed.
    if (/^readme\.md$/i.test(name) || !/\.md$/i.test(name)) continue;
    if (!FRAGMENT_RE.test(name)) {
      problems.push({
        file: name,
        problem:
          "filename will never be picked up by the assembler; name it <added|changed|fixed>-<slug>.md",
      });
      continue;
    }
    const body = readLF(join(dir, name)).replace(/\n+$/, "");
    if (!body.trim()) {
      problems.push({ file: name, problem: "fragment is empty" });
      continue;
    }
    if (!body.startsWith("- ")) {
      problems.push({
        file: name,
        problem: `must start with "- " (a finished Markdown bullet). Got: ${JSON.stringify(
          body.slice(0, 60),
        )}`,
      });
      continue;
    }
    const lines = body.split("\n");
    const problemsBefore = problems.length;
    for (let i = 1; i < lines.length; i++) {
      const line = lines[i];
      if (line === "" || line.startsWith("- ") || line.startsWith("  ")) continue;
      problems.push({
        file: name,
        problem: `line ${i + 1} is a wrapped continuation but isn't indented 2 spaces (the assembler concatenates verbatim): ${JSON.stringify(
          line.slice(0, 40),
        )}`,
      });
      break;
    }
    if (problems.length > problemsBefore) continue;
    const interrupter = bulletsOf(lines).some(
      (bullet) => (withoutQuotedSpans(bullet).match(/—/g) || []).length >= 2,
    );
    if (interrupter) {
      problems.push({
        file: name,
        problem: `two or more em-dashes in one bullet reads as a paired "— aside —" interrupter; recast with commas or parentheses. Quoted UI copy is exempt inside backticks, **bold**, or "quotes" (see changelog.d/README.md)`,
      });
    }
  }
  return problems;
}

/**
 * Render the `### Category` blocks for the non-empty groups (no version heading),
 * as an array of lines. Each block is `### Cat`, blank, its bullets, blank.
 */
export function renderGroupBlocks(groups) {
  const out = [];
  for (const cat of CATEGORY_ORDER) {
    const items = groups[cat];
    if (!items || items.length === 0) continue;
    out.push(`### ${cat}`, "");
    for (const item of items) out.push(...item.split("\n"));
    out.push("");
  }
  return out;
}

/** Render a whole section (`## [heading]` + its category blocks) as lines. */
export function renderSectionLines(heading, groups) {
  return [heading, "", ...renderGroupBlocks(groups)];
}

/** Total bullet count across all groups. */
export function countEntries(groups) {
  return CATEGORY_ORDER.reduce((n, c) => n + groups[c].length, 0);
}

// --- CHANGELOG.md assembly ----------------------------------------------------

/**
 * Locate "## [Unreleased]" in CHANGELOG.md's lines. Returns
 * `{ unreleasedIdx, nextIdx, body }` (`unreleasedIdx` is -1 when absent);
 * `body` is everything up to the next "## [" heading.
 */
export function findUnreleased(lines) {
  const unreleasedIdx = lines.findIndex((l) => /^##\s*\[Unreleased\]/i.test(l));
  if (unreleasedIdx === -1) return { unreleasedIdx, nextIdx: -1, body: [] };
  let nextIdx = lines.length;
  for (let i = unreleasedIdx + 1; i < lines.length; i++) {
    if (/^##\s*\[/.test(lines[i])) {
      nextIdx = i;
      break;
    }
  }
  return {
    unreleasedIdx,
    nextIdx,
    body: lines.slice(unreleasedIdx + 1, nextIdx),
  };
}

// Insert each category's new bullets at the end of that category's FIRST block
// in the pending body, leaving every existing line untouched. Creates a missing
// category heading in canonical Added→Changed→Fixed order.
export function mergeIntoBody(bodyLines, grp) {
  const out = [...bodyLines];
  for (const cat of CATEGORY_ORDER) {
    if (!grp[cat].length) continue;
    const bullets = grp[cat].flatMap((b) => b.split("\n"));
    const headingIdx = out.findIndex((l) =>
      new RegExp(`^###\\s+${cat}\\b`).test(l),
    );
    if (headingIdx !== -1) {
      let end = out.length;
      for (let i = headingIdx + 1; i < out.length; i++) {
        if (/^###\s+/.test(out[i]) || /^##\s+/.test(out[i])) {
          end = i;
          break;
        }
      }
      let at = end;
      while (at > headingIdx + 1 && out[at - 1].trim() === "") at--;
      // An empty block (a bare heading) still gets its blank line before the bullets.
      out.splice(at, 0, ...(at === headingIdx + 1 ? ["", ...bullets] : bullets));
    } else {
      const later = CATEGORY_ORDER.slice(CATEGORY_ORDER.indexOf(cat) + 1);
      let beforeIdx = -1;
      for (const lc of later) {
        const idx = out.findIndex((l) =>
          new RegExp(`^###\\s+${lc}\\b`).test(l),
        );
        if (idx !== -1) {
          beforeIdx = idx;
          break;
        }
      }
      if (beforeIdx !== -1) {
        out.splice(beforeIdx, 0, `### ${cat}`, "", ...bullets, "");
      } else {
        let at = out.length;
        while (at > 0 && out[at - 1].trim() === "") at--;
        out.splice(at, 0, "", `### ${cat}`, "", ...bullets);
      }
    }
  }
  return out;
}

// Empty pending body (steady state) or --no-merge: keep any existing content
// verbatim, then append freshly rendered category blocks.
export function appendToBody(bodyLines, grp) {
  const blocks = renderGroupBlocks(grp);
  // Strip surrounding blank lines to find real content.
  let start = 0;
  let end = bodyLines.length;
  while (start < end && bodyLines[start].trim() === "") start++;
  while (end > start && bodyLines[end - 1].trim() === "") end--;
  const content = bodyLines.slice(start, end);
  if (content.length === 0) return blocks.length ? ["", ...blocks] : ["", ""];
  return ["", ...content, "", ...blocks];
}

/**
 * The pending notes: the [Unreleased] body with the fragments merged in (the
 * same merge a stable release performs), as lines.
 */
export function pendingBody(bodyLines, grp, { noMerge = false } = {}) {
  const hasHeadings = bodyLines.some((l) => /^###\s+/.test(l));
  return hasHeadings && !noMerge
    ? mergeIntoBody(bodyLines, grp)
    : appendToBody(bodyLines, grp);
}

/** True when the lines hold a bullet: a body of bare `### Category` headings has no entries. */
export const hasEntries = (lines) => lines.some((l) => /^\s*[-*+] \S/.test(l));

/**
 * The version of CHANGELOG.md's newest stable section, or null when it has none.
 * The file lists versions newest first and only a stable release writes a section
 * into the tree, so this is the release a pre-release's notes count from.
 */
export function lastStableVersion(lines) {
  for (const line of lines) {
    const m = line.match(/^##\s*\[([^\]]+)\]/);
    if (m && parseVersion(m[1])?.channel === null) return m[1];
  }
  return null;
}

// What a pre-release installer does to PlugY.ini: keep in step with the
// PRERELEASE_ID block in installer/PlugY.nsi's Core section.
export const PRERELEASE_SETTINGS =
  "Its installer also sets two options in `PlugY.ini` at every install, including when the updater updates " +
  "an install the installer made: full session logs (`ActiveLogFile=1`), so your bug reports can include the " +
  "whole session, and the pre-release update channel (`UpdateChannel=prerelease`), so the game tells you " +
  "when the next pre-release is out. To log only PlugY's startup or hear only about releases, set them " +
  "back to `0` and `stable` after each install or update.";

/**
 * The paragraph a pre-release's notes open with, in its packaged CHANGELOG.md,
 * and in its GitHub release whenever those notes don't name a previous
 * pre-release. `base` is the version without its channel;
 * `lastStable` names the release the notes count from (null: none recorded).
 */
export const prereleaseIntro = (base, withEntries, lastStable) => {
  const since = lastStable ?? "the last stable release";
  const scope = withEntries
    ? `these notes cover everything since ${since}`
    : `no changes are recorded since ${since}`;
  return `Pre-release of ${base} for testing; ${scope}. ${PRERELEASE_SETTINGS}`;
};

/**
 * Repoint [Unreleased] to compare against the new tag and insert the new
 * version's compare link. The previous tag is captured verbatim from the
 * existing [Unreleased] def. Only a stable release writes CHANGELOG.md back to
 * the tree, so the committed def always names the last stable release, and the
 * first release compares against the upstream, non-semver v14.03.
 */
export function rewriteCompareLinks(fullText, ver) {
  const m = fullText.match(
    // [ \t]*, not \s*: under the m flag \s would eat the newline after the def.
    /^\[Unreleased\]:[ \t]*(\S+?)\/compare\/(\S+?)\.\.\.HEAD[ \t]*$/m,
  );
  if (!m) {
    throw new Error(
      "could not find the '[Unreleased]: …/compare/<tag>...HEAD' link definition",
    );
  }
  const [, base, prevTag] = m;
  const replacement =
    `[Unreleased]: ${base}/compare/v${ver}...HEAD\n` +
    `[${ver}]: ${base}/compare/${prevTag}...v${ver}`;
  return fullText.replace(m[0], () => replacement);
}

/**
 * Promote the pending notes to "## [ver] - date" with a fresh empty
 * "## [Unreleased]" above, then rewrite the compare links. `intro`, when given,
 * is a paragraph placed under the new heading. Pure: takes and returns
 * CHANGELOG.md's LF text.
 */
export function assembleChangelog(text, ver, groups, { noMerge = false, date, intro }) {
  const lines = text.split("\n");
  const { unreleasedIdx, nextIdx, body } = findUnreleased(lines);
  if (unreleasedIdx === -1) {
    throw new Error("could not find '## [Unreleased]' in CHANGELOG.md");
  }
  const esc = ver.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const existing = new RegExp(`^##\\s*\\[${esc}\\]`);
  if (lines.some((l) => existing.test(l))) {
    throw new Error(`CHANGELOG.md already has a '## [${ver}]' section; choose another version`);
  }
  const newBody = pendingBody(body, groups, { noMerge });
  const newRegion = [
    "## [Unreleased]",
    "",
    `## [${ver}] - ${date}`,
    ...(intro ? ["", intro] : []),
    ...newBody,
  ];
  const assembled = [
    ...lines.slice(0, unreleasedIdx),
    ...newRegion,
    ...lines.slice(nextIdx),
  ].join("\n");
  return rewriteCompareLinks(assembled, ver);
}
