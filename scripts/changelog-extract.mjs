#!/usr/bin/env node
// Prints the release notes for a tag, for use as the GitHub Release body. A
// stable tag gets its CHANGELOG.md section. A pre-release tag gets everything
// since the last stable release: the [Unreleased] body merged with the pending
// changelog.d fragments (a pre-release never consumes them). When an earlier
// pre-release since that stable release has a tag, the fragments whose file
// names its tree lacked lead under "New since <it>", and the whole list follows
// in a collapsed <details> block. The whole list stands alone without that tag,
// when git can't tell, or when every entry is new; when none is, the opening
// paragraph says so. A stable tag with no section falls back to a pointer line;
// the script always exits 0 so a notes problem never fails the release. Usage:
//   node scripts/changelog-extract.mjs v14.4.0
//   node scripts/changelog-extract.mjs v14.4.0-rc.1
import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import {
  countEntries,
  findUnreleased,
  hasEntries,
  lastStableVersion,
  newSinceGroups,
  pendingBody,
  PRERELEASE_SETTINGS,
  prereleaseIntro,
  previousPrerelease,
  readFragments,
  renderGroupBlocks,
} from "./changelog-lib.mjs";
import { parseVersion } from "./version-lib.mjs";

// GitHub renders a release body like a comment, turning every newline into a
// line break, so CHANGELOG.md's wrapped lines would break mid-sentence there.
// The body joins wrapped lines back together only in the shape changelog
// entries take: a paragraph, or a list item with one space after its marker
// ("- ", "1. "), starting in column 0 and continued by lines of plain text. Every
// other line is kept as written, so what this doesn't recognise renders as it
// does today. From the first code fence or raw HTML block (<pre>, a comment)
// on, nothing is joined: their blank lines and look-alike closers make pairing
// them guesswork. CHANGELOG.md itself stays wrapped.

// CommonMark's blank lines and line-edge whitespace are spaces and tabs only; other spaces are content.
// The trims scan characters: an end-anchored regex retries every start and goes quadratic on long runs.
const BLANK_RE = /^[ \t]*$/;
const isSpace = (c) => c === " " || c === "\t";
const trimStartSpacesTabs = (s) => {
  let a = 0;
  while (a < s.length && isSpace(s[a])) a++;
  return s.slice(a);
};
const trimSpacesTabs = (s) => {
  const t = trimStartSpacesTabs(s);
  let b = t.length;
  while (b > 0 && isSpace(t[b - 1])) b--;
  return t.slice(0, b);
};

// The quote and list markers before a line's content.
const CONTAINER_RE = /^([ \t]*(>[ \t]?|[-*+]([ \t]+|$)|\d{1,9}[.)]([ \t]+|$)))*/;
const QUOTE_RE = /^([ \t]*>[ \t]?)*/;
const FENCE_RE = /^[ \t]*(`{3,}|~{3,})(.*)$/;
const MATH_RE = /^[ \t]*\$\$[ \t]*$/; // a GitHub math block, whose TeX comments run to the end of a line
const RAW_HTML_RE = /^[ \t]*(<(pre|script|style|textarea)([ \t>]|$)|<!--|<\?|<![A-Za-z]|<!\[CDATA\[)/i;
// A link label may hold escaped brackets.
const LABEL = String.raw`\[(?:\\.|[^\]\\])*\]:`;
// A list item that a continuation may join: one space after the marker, then content.
const ITEM_RE = /^([-*+]|\d{1,9}[.)]) (?=[^ \t])/;
// Content that opens or belongs to some other block: a heading, list item, quote, table, HTML,
// alert or link definition.
const NOT_PLAIN_RE = new RegExp(
  String.raw`^(#{1,6}([ \t]|$)|[-*+]([ \t]|$)|\d{1,9}[.)]([ \t]|$)|>|\||<|\[!|` + String.raw`${LABEL}|[^[]*\]:)`,
);
const UNDERLINE_RE = /^[ \t]*(=+|-+)[ \t]*$/;
const RULE_RE = /^[ \t]*([-*_])([ \t]*\1){2,}[ \t]*$/;
// A list item or quote moves the indent its underlines and rules may have, so any indent counts.
const isUnderline = (line) => UNDERLINE_RE.test(line) || UNDERLINE_RE.test(line.replace(CONTAINER_RE, ""));
const isRule = (line) => RULE_RE.test(line) || RULE_RE.test(line.replace(CONTAINER_RE, ""));
const isPlain = (content) =>
  content !== "" && !NOT_PLAIN_RE.test(content) && !isUnderline(content) && !isRule(content);
// A delimiter row needs a pipe or a colon: a bare run of dashes is a setext underline or a rule.
const isTableDelimiter = (line) =>
  line !== undefined &&
  [line, line.replace(QUOTE_RE, "")].some(
    (l) => /^[ \t|:-]+$/.test(l) && l.includes("-") && (l.includes("|") || l.includes(":")),
  );
// A table, an HTML block or a link definition may hold line breaks of its own, so no join enters
// one until the next blank line.
const BLOCK_CONTENT_RE = new RegExp(String.raw`^[ \t]*([|<]|${LABEL}|[^[]*\]:)`);
const opensBlock = (line, next) =>
  isTableDelimiter(line) || isTableDelimiter(next) || BLOCK_CONTENT_RE.test(line.replace(CONTAINER_RE, ""));
// A link destination in <...> can't span a line break, so joining one would make a broken link work.
const OPEN_ANGLE_RE = /\]\([ \t]*<(?:\\.|[^>\\])*$/;
// Inline <pre>, <code> and <textarea> keep their line breaks; whether one is still open after `part`.
const leavesRawOpen = (part, open) => {
  for (const m of part.matchAll(/<(\/?)(pre|code|textarea)\b/gi)) open = m[1] === "";
  return open;
};
// The length of the backtick run of a code span still open after `part` (0: none). A code span keeps
// the next line's leading spaces, which a join trims; a run that never closes is counted as open too.
const leavesTicksOpen = (part, open) => {
  for (let k = 0; k < part.length; ) {
    if (!open && part[k] === "\\") k += 2;
    else if (part[k] !== "`") k++;
    else {
      let n = 0;
      while (part[k + n] === "`") n++;
      if (!open) open = n;
      else if (n === open) open = 0;
      k += n;
    }
  }
  return open;
};

// A backtick fence's info string can't hold a backtick, so "```x``` text" is inline code.
const opensFence = (content) => {
  const m = content.match(FENCE_RE);
  return (m !== null && !(m[1][0] === "`" && m[2].includes("`"))) || MATH_RE.test(content);
};

/** `text` (LF) with each wrapped paragraph or list-item line joined onto the line it continues. */
export function unwrapMarkdown(text) {
  const lines = text.split("\n");
  // Lines with an underline-like line below them in their paragraph stay as written: a setext heading's
  // anchor is built from its text as written, and a rule there means more than one block.
  const underlined = new Array(lines.length + 1).fill(false);
  for (let j = lines.length - 1; j >= 0; j--)
    underlined[j] = !BLANK_RE.test(lines[j]) && (isUnderline(lines[j]) || underlined[j + 1]);
  // Each output line is kept as its parts, so a join touches only the newest part, never the whole line.
  const out = [];
  let joinable = false; // the last line is a paragraph or list-item line a plain continuation may join
  let block = false; // inside a table, an HTML block or a link definition: no joins until a blank line
  let openAngle = false; // the last line ends inside a link destination's <...>
  let rawOpen = false; // an inline <pre>, <code> or <textarea> is open in this paragraph
  let ticksOpen = 0; // a code span is open in this paragraph
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (opensFence(line.replace(CONTAINER_RE, "")) || RAW_HTML_RE.test(line.replace(CONTAINER_RE, ""))) {
      for (const rest of lines.slice(i)) out.push([rest]);
      break;
    }
    // An inline <pre>, <code> or <textarea> left open at a paragraph's end can run on through the
    // HTML after it, so it ends all joining like a fence.
    if (BLANK_RE.test(line) && rawOpen) {
      for (const rest of lines.slice(i)) out.push([rest]);
      break;
    }
    if (BLANK_RE.test(line)) {
      out.push([line]);
      joinable = false;
      block = false;
      ticksOpen = 0;
      continue;
    }
    if (opensBlock(line, lines[i + 1])) block = true;
    const hardBreak = line.endsWith("  ") || line.endsWith("\\");
    const rawBefore = rawOpen;
    const ticksBefore = ticksOpen;
    rawOpen = leavesRawOpen(line, rawBefore);
    ticksOpen = leavesTicksOpen(line, ticksBefore);
    const inSpan = rawBefore || ticksBefore > 0 || openAngle;
    if (joinable && !block && !underlined[i] && !inSpan && isPlain(trimSpacesTabs(line))) {
      // The previous part keeps its trailing space: inside a code span that crosses the break it is text.
      const body = hardBreak ? trimStartSpacesTabs(line) : trimSpacesTabs(line);
      out[out.length - 1].push(body);
      joinable = !hardBreak;
      openAngle = OPEN_ANGLE_RE.test(body);
      continue;
    }
    out.push([line]);
    const item = line.match(ITEM_RE);
    const content = item ? line.slice(item[0].length) : /^[^ \t]/.test(line) ? line : "";
    joinable = !block && !hardBreak && !underlined[i] && isPlain(content);
    openAngle = OPEN_ANGLE_RE.test(line);
  }
  return out.map((parts) => parts.join(" ")).join("\n");
}

// GitHub rejects a release body over 125,000 characters, and Linux caps any
// single env var (a workflow step receiving the body as an input) at 128 KiB
// (MAX_ARG_STRLEN). When the notes don't fit, keep the top and link out to the
// full file. Budget stays under both limits with margin.
export const MAX_BYTES = 120_000;
const byteLen = (s) => Buffer.byteLength(s, "utf8");

const pointer = (tag) => `See the CHANGELOG for ${tag || "this release"}.`;

// The section under "## [version]" (with or without a trailing date), trimmed.
// The compare-link definitions closing the file belong to no section, so the
// oldest section stops before them.
function stableSection(lines, version) {
  const esc = version.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const header = new RegExp(`^##\\s*\\[${esc}\\]`);
  const start = lines.findIndex((l) => header.test(l));
  if (start === -1) return "";
  let end = lines.length;
  for (let i = start + 1; i < lines.length; i++) {
    if (/^##\s*\[/.test(lines[i]) || /^\[[^\]]+\]:\s*\S/.test(lines[i])) {
      end = i;
      break;
    }
  }
  return lines
    .slice(start + 1, end)
    .join("\n")
    .trim();
}

/**
 * Build the release body for `tag`. Pure: `changelogText` is CHANGELOG.md's
 * text (null when unreadable) and `groups` the pending fragments, used only
 * for a pre-release tag. `delta`, also pre-release only, is
 * `{ since: <previous pre-release version>, groups: <its newSinceGroups> }`, or
 * null when there is no previous pre-release to count from. `env` supplies
 * GITHUB_REPOSITORY / GITHUB_SERVER_URL for the truncation footer's link.
 */
export function buildReleaseNotes({ tag, changelogText, groups, delta, env = {} }) {
  if (changelogText === null || changelogText === undefined) {
    return "See CHANGELOG.md for release notes.\n";
  }
  const version = (tag ?? "").replace(/^v/, "");
  const parsed = parseVersion(version);
  const lines = changelogText.split(/\r?\n/);

  let notes;
  if (parsed?.channel) {
    const { body } = findUnreleased(lines);
    const fragments = groups ?? { Added: [], Changed: [], Fixed: [] };
    const pending = pendingBody(body, fragments).join("\n").trim();
    // No pointer either way: the repo's CHANGELOG.md never gets a pre-release section.
    const withEntries = hasEntries(pending.split("\n"));
    const lastStable = lastStableVersion(lines);
    const stableLabel = lastStable ?? "the last stable release";
    // Only fragments can be new: bullets written by hand into [Unreleased] have no
    // file name to compare, so they appear in the whole list alone.
    const newCount = delta ? countEntries(delta.groups) : 0;
    const allNew = newCount === countEntries(fragments) && !hasEntries(body);
    if (!withEntries) {
      notes = prereleaseIntro(parsed.base, false, lastStable);
    } else if (!delta || allNew) {
      notes = `${prereleaseIntro(parsed.base, true, lastStable)}\n\n${pending}`;
    } else if (newCount === 0) {
      const intro =
        `Pre-release of ${parsed.base} for testing; no entries were added since ${delta.since}, ` +
        `and these notes cover everything since ${stableLabel}. ${PRERELEASE_SETTINGS}`;
      notes = `${intro}\n\n${pending}`;
    } else {
      const intro =
        `Pre-release of ${parsed.base} for testing. What's new since ${delta.since} comes first; ` +
        `every change since ${stableLabel} is listed below it. ${PRERELEASE_SETTINGS}`;
      const fresh = renderGroupBlocks(delta.groups).join("\n").trim();
      const count = pending.split("\n").filter((l) => /^[-*+] \S/.test(l)).length;
      notes = [
        intro,
        "",
        `## New since ${delta.since}`,
        "",
        fresh,
        "",
        `## Every change since ${stableLabel}`,
        "",
        "<details>",
        `<summary>Show all ${count} changes</summary>`,
        "",
        pending,
        "",
        "</details>",
      ].join("\n");
    }
  } else {
    notes = stableSection(lines, version) || pointer(tag);
  }
  notes = unwrapMarkdown(notes);

  if (byteLen(`${notes}\n`) <= MAX_BYTES) return `${notes}\n`;

  // GITHUB_REPOSITORY / GITHUB_SERVER_URL are always set in Actions; fall back
  // to a bare filename when run locally.
  const repo = env.GITHUB_REPOSITORY;
  const server = env.GITHUB_SERVER_URL ?? "https://github.com";
  const changelogUrl = repo
    ? `${server}/${repo}/blob/${tag}/CHANGELOG.md`
    : "CHANGELOG.md";
  const footer = `\n\n---\n\n_Release notes truncated; see the [full changelog](${changelogUrl}) for the rest._\n`;
  const budget = MAX_BYTES - byteLen(footer);

  // Keep whole lines only, so the truncated markdown still renders cleanly.
  const kept = [];
  let used = 0;
  for (const line of notes.split("\n")) {
    const chunkBytes = byteLen(`${line}\n`);
    if (used + chunkBytes > budget) break;
    kept.push(line);
    used += chunkBytes;
  }
  // A <details> left open would swallow the footer, so each open level (blocks may
  // nest) is closed within the budget.
  const closer = "\n\n</details>";
  const openDepth = () => {
    let depth = 0;
    for (const l of kept) {
      if (l === "<details>") depth++;
      else if (l === "</details>" && depth > 0) depth--;
    }
    return depth;
  };
  while (openDepth() > 0 && used + openDepth() * byteLen(closer) > budget) {
    used -= byteLen(`${kept.pop()}\n`);
  }
  return `${kept.join("\n").trimEnd()}${closer.repeat(openDepth())}${footer}`;
}

// Git's stdout only, never its stderr: the release workflow takes this script's
// stdout as the body. The ceiling keeps git from answering for a repository that
// encloses a tree without history, and the variables a git hook exports would
// point it at another one. Null when git fails in any way.
function git(root, args) {
  const env = { ...process.env, GIT_CEILING_DIRECTORIES: dirname(root) };
  for (const name of ["GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR"]) delete env[name];
  const r = spawnSync("git", args, { cwd: root, encoding: "utf8", stdio: "pipe", env });
  return r.error || r.status !== 0 ? null : r.stdout;
}

// The previous pre-release and the fragments new since its tree, or null when
// there is none or git can't tell: the notes are then the whole list alone. A
// failed git call says so on stderr. A checkout missing the earlier tags reads as
// a first pre-release, with nothing on stderr: release.yml fetches every tag.
function findDelta(root, version, changelogText, entries) {
  const failed = (what) => {
    process.stderr.write(`changelog-extract: git could not ${what}; the notes list every entry.\n`);
    return null;
  };
  try {
    const tags = git(root, ["tag", "--list", "v*"]);
    if (tags === null) return failed("list tags");
    const since = previousPrerelease(tags, version, lastStableVersion(changelogText.split(/\r?\n/)));
    if (since === null) return null;
    // -z leaves non-ASCII names unquoted.
    const listing = git(root, ["ls-tree", "-z", "--name-only", `v${since}`, "--", "changelog.d/"]);
    if (listing === null) return failed(`read the tree of v${since}`);
    const names = listing
      .split("\0")
      .filter((p) => p !== "")
      .map((p) => p.replace(/^changelog\.d\//, ""));
    return { since, groups: newSinceGroups(entries, names) };
  } catch {
    return null;
  }
}

function main() {
  const tag = process.argv[2] ?? "";
  const root = join(dirname(fileURLToPath(import.meta.url)), "..");
  let out;
  try {
    let changelogText = null;
    try {
      changelogText = readFileSync(join(root, "CHANGELOG.md"), "utf8");
    } catch {
      // unreadable: buildReleaseNotes prints the generic pointer
    }
    // Only a pre-release reads the fragments or asks git, so a malformed fragment
    // can't touch a stable release's notes.
    const version = tag.replace(/^v/, "");
    let groups = null;
    let delta = null;
    if (parseVersion(version)?.channel) {
      const fragments = readFragments(join(root, "changelog.d"));
      groups = fragments.groups;
      if (changelogText !== null) delta = findDelta(root, version, changelogText, fragments.entries);
    }
    out = buildReleaseNotes({ tag, changelogText, groups, delta, env: process.env });
  } catch (err) {
    // A malformed fragment must not fail the release; the pointer still ships.
    process.stderr.write(`changelog-extract: ${err.message}\n`);
    out = `${pointer(tag)}\n`;
  }
  process.stdout.write(out);
}

if (
  process.argv[1] &&
  resolve(process.argv[1]) === fileURLToPath(import.meta.url)
) {
  main();
}
