// changelog-extract: release bodies for stable and pre-release tags, the
// pointer fallbacks, and the size cap. buildReleaseNotes is pure, so no file
// or git state is involved; the one end-to-end test runs the script from a
// throwaway repository, never this one.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { copyFileSync, mkdirSync, mkdtempSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

import { buildReleaseNotes, MAX_BYTES, unwrapMarkdown } from "./changelog-extract.mjs";
import { PRERELEASE_SETTINGS, readFragments } from "./changelog-lib.mjs";

const intro = (line) => `${line} ${PRERELEASE_SETTINGS}`;

const empty = { Added: [], Changed: [], Fixed: [] };

const CHANGELOG = [
  "# Changelog",
  "",
  "## [Unreleased]",
  "",
  "### Fixed",
  "",
  "- Hand-written fix.",
  "",
  "## [14.4.0] - 2026-10-08",
  "",
  "### Added",
  "",
  "- Released feature.",
  "",
  "## [14.3.0] - 2021-09-01",
  "",
  "Upstream baseline.",
  "",
  "[Unreleased]: https://github.com/theBGuy/PlugY4Kolbot/compare/v14.4.0...HEAD",
  "",
].join("\n");

test("a stable tag prints its section body", () => {
  const out = buildReleaseNotes({ tag: "v14.4.0", changelogText: CHANGELOG, groups: null });
  assert.equal(out, "### Added\n\n- Released feature.\n");
  assert.equal(
    buildReleaseNotes({ tag: "v14.3.0", changelogText: CHANGELOG.replace(/\n/g, "\r\n"), groups: null }),
    "Upstream baseline.\n",
  );
});

test("a pre-release tag prints the intro plus Unreleased merged with fragments", () => {
  const out = buildReleaseNotes({
    tag: "v14.5.0-rc.1",
    changelogText: CHANGELOG,
    groups: { Added: ["- Pending feature."], Changed: [], Fixed: ["- Pending fix."] },
  });
  assert.equal(
    out,
    [
      intro("Pre-release of 14.5.0 for testing; these notes cover everything since 14.4.0."),
      "",
      "### Added",
      "",
      "- Pending feature.",
      "",
      "### Fixed",
      "",
      "- Hand-written fix.",
      "- Pending fix.",
      "",
    ].join("\n"),
  );
});

test("a pre-release with an empty Unreleased body renders the fragments alone", () => {
  const text = CHANGELOG.replace("### Fixed\n\n- Hand-written fix.\n\n", "");
  const out = buildReleaseNotes({
    tag: "v14.5.0-beta.2",
    changelogText: text,
    groups: { Added: [], Changed: ["- Pending change,\n  wrapped."], Fixed: [] },
  });
  assert.equal(
    out,
    intro("Pre-release of 14.5.0 for testing; these notes cover everything since 14.4.0.") +
      "\n\n### Changed\n\n- Pending change, wrapped.\n",
  );
});

test("the pre-release intro counts from the newest stable section", () => {
  const groups = { Added: [], Changed: [], Fixed: ["- A fix."] };
  // A pre-release section (only packaged files have one) and a non-semver heading are skipped.
  const withRc = CHANGELOG.replace(
    "## [14.4.0] - 2026-10-08",
    "## [14.5.0-rc.1] - 2026-10-09\n\n## [Old stuff]\n\n## [14.4.0] - 2026-10-08",
  );
  assert.match(
    buildReleaseNotes({ tag: "v14.5.0-rc.2", changelogText: withRc, groups }),
    /^Pre-release of 14\.5\.0 for testing; these notes cover everything since 14\.4\.0\. Its installer /,
  );
  const noStable = "# Changelog\n\n## [Unreleased]\n";
  assert.equal(
    buildReleaseNotes({ tag: "v1.0.0-rc.1", changelogText: noStable, groups }),
    intro("Pre-release of 1.0.0 for testing; these notes cover everything since the last stable release.") +
      "\n\n### Fixed\n\n- A fix.\n",
  );
  assert.equal(
    buildReleaseNotes({ tag: "v1.0.0-rc.1", changelogText: noStable, groups: empty }),
    intro("Pre-release of 1.0.0 for testing; no changes are recorded since the last stable release.") + "\n",
  );
});

test("release bodies put each wrapped bullet and paragraph on one line", () => {
  const text = CHANGELOG.replace(
    "- Released feature.",
    "- Released feature,\n  wrapped once\n  and twice.\n- Second.\n\nA paragraph\nwrapped.",
  );
  assert.equal(
    buildReleaseNotes({ tag: "v14.4.0", changelogText: text, groups: null }),
    "### Added\n\n- Released feature, wrapped once and twice.\n- Second.\n\nA paragraph wrapped.\n",
  );
});

test("unwrapping joins only paragraph and list-item continuations", () => {
  const F = "```";
  // Joining any line of these changes what GitHub renders, so each comes back as it was.
  const unchanged = [
    `- ${F}\n  code a\n  code b\n  ${F}\n\nafter\nwrap`, // a fence on a list item stops all joining
    `> ${F}\nx\ny`,
    `a\n    ${F}\nb\nc\n\n${F}\ncode1\ncode2\n${F}`,
    `${F}\nexample:\n    ${F}\nline a\nline b\n${F}`,
    "# H\n    code1\n    code2", // indented code right after a heading or a rule
    "***\n    code1\n    code2",
    `${F}\nx\n${F}\n    code1\n    code2`, // a closed fence still ends all joining
    "-     code\nnext", // indented code inside a list item or quote
    ">     code\nnext",
    "Intro text\na | b\n--|--\n1 | 2", // a table header without a leading pipe
    "> a | b\n> --- | ---\n> c | d\ne", // a table in a quote doesn't continue lazily
    "[a]:\n/url\n'title'\nnext", // a link definition over several lines
    "[foo\nbar]: /url\n'title'\nnext\n\n[foo bar]",
    "see [x](<a\nb.md>)", // a <...> link destination can't span a line break
    "[a]( <b\nc>)",
    "[a\\]b]: /u\nfoo\n\n[a\\]b]", // a link label with an escaped bracket
    "a\n:-\nb", // a one-column table needs no pipe
    "foo <pre>a\nb</pre> c", // inline raw HTML that keeps its line breaks
    "foo <code>a\nb\nc</code> d",
    "$$\nx % c\ny\n$$", // a math block's TeX comment would swallow the next line
    `| a |\n| - |\n| b |\n${String.fromCharCode(0xa0)}\nfoo\nbar`, // a no-break space doesn't end a table
    "Title\n=",
    "Title\n--",
    "more words\n10. ten\n   wrapped\n=", // a setext heading over several lines keeps its anchor
    "- item\n    ===", // a list item moves the indent its underlines and rules may have
    "- item\n    ***",
    "> para\nwrap\n> ===",
    "<pre>\nline1\nline2\n</pre>",
    '[foo]: /url\n"title"\nnext text',
    "> # heading\nlazy",
    "-\nfoo",
    "1.\nfoo",
    "text\n# Heading",
  ];
  for (const input of unchanged) assert.equal(unwrapMarkdown(input), input, JSON.stringify(input));
  // A GitHub alert's marker must stand alone on its line.
  assert.equal(unwrapMarkdown("> [!NOTE]\nlazy"), "> [!NOTE]\nlazy");
  assert.equal(
    unwrapMarkdown(`${F}x${F} text\nwrapped\n\n${F}\ncode1\ncode2\n${F}`),
    `${F}x${F} text wrapped\n\n${F}\ncode1\ncode2\n${F}`,
  );
  const cases = [
    // [input, expected]
    ["### Heading\ntext\nmore", "### Heading\ntext more"],
    ["- a\n- b\n* c\n+ d", "- a\n- b\n* c\n+ d"],
    // Only column-0 paragraphs and list items are joined; nested items and quotes keep their lines.
    ["- a\n  - nested\n    more", "- a\n  - nested\n    more"],
    ["1. one\n   cont\n2) two", "1. one cont\n2) two"],
    ["> quote\n> more\nlazy", "> quote\n> more\nlazy"],
    // Only spaces and tabs are trimmed; other spaces are content.
    [`foo\n${String.fromCharCode(0x3000)}bar`, `foo ${String.fromCharCode(0x3000)}bar`],
    [`foo${String.fromCharCode(0xa0)}\nbar`, `foo${String.fromCharCode(0xa0)} bar`],
    ["text\n```js\nline one\nline two\n```\nafter", "text\n```js\nline one\nline two\n```\nafter"],
    ["~~~~\na\nb\n~~~\nc\n~~~~", "~~~~\na\nb\n~~~\nc\n~~~~"],
    ["- item\n    ```\n    a\n    b\n    ```", "- item\n    ```\n    a\n    b\n    ```"],
    ["| a | b |\n| --- | --- |\n| 1 | 2 |\n| 3 | 4 |", "| a | b |\n| --- | --- |\n| 1 | 2 |\n| 3 | 4 |"],
    ["a | b\n--- | ---\n1 | 2\n3 | 4\n\nafter\nwrap", "a | b\n--- | ---\n1 | 2\n3 | 4\n\nafter wrap"],
    ["Title\n===\ntext\n\n---\n\nmore", "Title\n===\ntext\n\n---\n\nmore"],
    ["Sub\n---", "Sub\n---"],
    ["two spaces  \nnext\nwrap", "two spaces  \nnext wrap"],
    ["backslash\\\nnext", "backslash\\\nnext"],
    ["para\n\n    code one\n    code two\n\nend", "para\n\n    code one\n    code two\n\nend"],
    ["para\n    indented continuation", "para indented continuation"],
    ["[x]: https://example.com\n[y]: https://example.org", "[x]: https://example.com\n[y]: https://example.org"],
    ["<details>\n<summary>x</summary>\ntext", "<details>\n<summary>x</summary>\ntext"],
    // The earlier line keeps its trailing space; a code span crossing the break keeps the break.
    ["trailing space \nnext", "trailing space  next"],
    ["foo `a \nb` bar\nmore", "foo `a \nb` bar more"],
    // An inline raw tag left open at a paragraph's end stops joining for the rest of the body.
    ["foo <pre>a\n\nb\nc", "foo <pre>a\n\nb\nc"],
    ["", ""],
  ];
  for (const [input, expected] of cases) assert.equal(unwrapMarkdown(input), expected, JSON.stringify(input));
});

test("nothing found falls back to a pointer line", () => {
  assert.equal(
    buildReleaseNotes({ tag: "v9.9.9", changelogText: CHANGELOG, groups: null }),
    "See the CHANGELOG for v9.9.9.\n",
  );
  assert.equal(
    buildReleaseNotes({ tag: "", changelogText: CHANGELOG, groups: null }),
    "See the CHANGELOG for this release.\n",
  );
  assert.equal(
    buildReleaseNotes({ tag: "v14.4.0", changelogText: null, groups: null }),
    "See CHANGELOG.md for release notes.\n",
  );
  const bare = CHANGELOG.replace("### Fixed\n\n- Hand-written fix.\n\n", "");
  // A pre-release with nothing pending prints the intro alone: the repository's
  // CHANGELOG.md never gets a section for it that a pointer could name.
  const nothingPending = intro("Pre-release of 14.5.0 for testing; no changes are recorded since 14.4.0.") + "\n";
  assert.equal(buildReleaseNotes({ tag: "v14.5.0-rc.1", changelogText: bare, groups: empty }), nothingPending);
  assert.equal(buildReleaseNotes({ tag: "v14.5.0-rc.1", changelogText: bare, groups: null }), nothingPending);
  // Bare category headings with no bullets are not notes either.
  const headingsOnly = bare.replace("## [Unreleased]\n\n", "## [Unreleased]\n\n### Added\n\n### Fixed\n\n");
  assert.notEqual(headingsOnly, bare);
  assert.equal(
    buildReleaseNotes({ tag: "v14.5.0-rc.1", changelogText: headingsOnly, groups: empty }),
    nothingPending,
  );
  // ...while a single bullet under them is.
  assert.match(
    buildReleaseNotes({ tag: "v14.5.0-rc.1", changelogText: headingsOnly, groups: { ...empty, Fixed: ["- One fix."] } }),
    /cover everything since 14\.4\.0\. Its installer [^\n]*\n\n### Added\n\n### Fixed\n\n- One fix\.\n$/,
  );
});

test("oversized notes keep whole lines under the cap and link the full changelog", () => {
  const bullets = Array.from({ length: 4000 }, (_, i) => `- Entry number ${i} with some padding text.`);
  const text = CHANGELOG.replace("- Released feature.", bullets.join("\n"));
  const out = buildReleaseNotes({
    tag: "v14.4.0",
    changelogText: text,
    groups: null,
    env: { GITHUB_REPOSITORY: "theBGuy/PlugY4Kolbot" },
  });
  assert.ok(Buffer.byteLength(out, "utf8") <= MAX_BYTES);
  assert.match(out, /truncated; see the \[full changelog\]\(https:\/\/github\.com\/theBGuy\/PlugY4Kolbot\/blob\/v14\.4\.0\/CHANGELOG\.md\)/);
  const kept = out.slice(0, out.indexOf("\n\n---"));
  assert.ok(kept.split("\n").every((l) => l === "" || l === "### Added" || /^- Entry number \d+ with some padding text\.$/.test(l)));
});

// --- pre-release deltas -------------------------------------------------------

const deltaIntro = (base, prev, stable) =>
  intro(`Pre-release of ${base} for testing. What's new since ${prev} comes first; every change since ${stable} is listed below it.`);

const PENDING = { Added: ["- Old feature.", "- New feature,\n  wrapped."], Changed: [], Fixed: ["- Old fix."] };
const NEW_ONLY = { Added: ["- New feature,\n  wrapped."], Changed: [], Fixed: [] };

test("a pre-release with new entries since the previous one leads with them and folds the whole list", () => {
  const out = buildReleaseNotes({
    tag: "v14.5.0-rc.2",
    changelogText: CHANGELOG,
    groups: PENDING,
    delta: { since: "14.5.0-rc.1", groups: NEW_ONLY },
  });
  assert.equal(
    out,
    [
      deltaIntro("14.5.0", "14.5.0-rc.1", "14.4.0"),
      "",
      "## New since 14.5.0-rc.1",
      "",
      "### Added",
      "",
      "- New feature, wrapped.",
      "",
      "## Every change since 14.4.0",
      "",
      "<details>",
      // The hand-written [Unreleased] bullet counts here, never among the new ones.
      "<summary>Show all 4 changes</summary>",
      "",
      "### Added",
      "",
      "- Old feature.",
      "- New feature, wrapped.",
      "",
      "### Fixed",
      "",
      "- Hand-written fix.",
      "- Old fix.",
      "",
      "</details>",
      "",
    ].join("\n"),
  );
  // Without the previous pre-release the same input renders today's whole list.
  for (const delta of [null, undefined]) {
    assert.equal(
      buildReleaseNotes({ tag: "v14.5.0-rc.2", changelogText: CHANGELOG, groups: PENDING, delta }),
      buildReleaseNotes({ tag: "v14.5.0-rc.2", changelogText: CHANGELOG, groups: PENDING }),
    );
  }
});

test("the delta keeps <details> and <summary> on their own lines while unwrapping the bullets inside", () => {
  const groups = { Added: [], Changed: ["- Pending change,\n  wrapped.", "- Older change,\n  wrapped too."], Fixed: [] };
  const out = buildReleaseNotes({
    tag: "v14.5.0-beta.3",
    changelogText: CHANGELOG,
    groups,
    delta: { since: "14.5.0-beta.2", groups: { Added: [], Changed: ["- Pending change,\n  wrapped."], Fixed: [] } },
  });
  const lines = out.split("\n");
  assert.equal(lines[lines.indexOf("<details>") + 1], "<summary>Show all 3 changes</summary>");
  assert.equal(lines[lines.indexOf("<details>") + 2], "");
  const inside = lines.slice(lines.indexOf("<details>"));
  assert.ok(inside.includes("- Pending change, wrapped."));
  assert.ok(inside.includes("- Older change, wrapped too."));
  assert.equal(lines.filter((l) => l === "- Pending change, wrapped.").length, 2);
  assert.ok(out.endsWith("\n\n</details>\n"));
});

test("a pre-release whose previous one already listed every entry says so and keeps the whole list", () => {
  const out = buildReleaseNotes({
    tag: "v14.5.0-rc.2",
    changelogText: CHANGELOG,
    groups: PENDING,
    delta: { since: "14.5.0-rc.1", groups: empty },
  });
  const today = buildReleaseNotes({ tag: "v14.5.0-rc.2", changelogText: CHANGELOG, groups: PENDING });
  const reworded = intro(
    "Pre-release of 14.5.0 for testing; no entries were added since 14.5.0-rc.1, and these notes cover everything since 14.4.0.",
  );
  assert.equal(out, reworded + today.slice(today.indexOf("\n\n")));
  assert.doesNotMatch(out, /<details>|## New since/);
  // With no stable release recorded, the intro names none.
  const noStable = "# Changelog\n\n## [Unreleased]\n";
  assert.match(
    buildReleaseNotes({
      tag: "v1.0.0-rc.2",
      changelogText: noStable,
      groups: PENDING,
      delta: { since: "1.0.0-rc.1", groups: empty },
    }),
    /^Pre-release of 1\.0\.0 for testing; no entries were added since 1\.0\.0-rc\.1, and these notes cover everything since the last stable release\. Its installer /,
  );
});

test("a pre-release whose every entry is new, or that has none, renders as it did without a previous one", () => {
  const bare = CHANGELOG.replace("### Fixed\n\n- Hand-written fix.\n\n", "");
  // Every entry new: the whole list is the delta.
  const allNew = buildReleaseNotes({ tag: "v14.5.0-rc.2", changelogText: bare, groups: PENDING, delta: { since: "14.5.0-rc.1", groups: PENDING } });
  assert.equal(allNew, buildReleaseNotes({ tag: "v14.5.0-rc.2", changelogText: bare, groups: PENDING }));
  assert.doesNotMatch(allNew, /<details>/);
  // ...unless a hand-written [Unreleased] bullet makes the new fragments a strict part of the list.
  const withHandWritten = buildReleaseNotes({
    tag: "v14.5.0-rc.2",
    changelogText: CHANGELOG,
    groups: PENDING,
    delta: { since: "14.5.0-rc.1", groups: PENDING },
  });
  assert.ok(withHandWritten.startsWith(`${deltaIntro("14.5.0", "14.5.0-rc.1", "14.4.0")}\n\n## New since 14.5.0-rc.1\n\n`));
  assert.match(withHandWritten, /\n<summary>Show all 4 changes<\/summary>\n/);
  // No entries at all.
  const nothing = intro("Pre-release of 14.5.0 for testing; no changes are recorded since 14.4.0.") + "\n";
  assert.equal(
    buildReleaseNotes({ tag: "v14.5.0-rc.2", changelogText: bare, groups: empty, delta: { since: "14.5.0-rc.1", groups: empty } }),
    nothing,
  );
  // A stable tag ignores a delta.
  assert.equal(
    buildReleaseNotes({ tag: "v14.4.0", changelogText: CHANGELOG, groups: null, delta: { since: "14.4.0-rc.1", groups: NEW_ONLY } }),
    "### Added\n\n- Released feature.\n",
  );
});

test("truncating a delta closes its <details> block within the cap", () => {
  const old = Array.from({ length: 4000 }, (_, i) => `- Entry number ${i} with some padding text.`);
  // Each pad moves the cut, so some cuts leave too little room for the closing tag.
  for (let pad = 0; pad < 60; pad++) {
    const fresh = `- New${"x".repeat(pad)}.`;
    const out = buildReleaseNotes({
      tag: "v14.5.0-rc.2",
      changelogText: CHANGELOG,
      groups: { Added: [fresh], Changed: [], Fixed: old },
      delta: { since: "14.5.0-rc.1", groups: { Added: [fresh], Changed: [], Fixed: [] } },
    });
    assert.ok(Buffer.byteLength(out, "utf8") <= MAX_BYTES, `pad ${pad}`);
    const lines = out.split("\n");
    assert.equal(lines.filter((l) => l === "<details>").length, 1, `pad ${pad}`);
    assert.equal(lines.filter((l) => l === "</details>").length, 1, `pad ${pad}`);
    assert.match(out, /\n\n<\/details>\n\n---\n\n_Release notes truncated; see the \[full changelog\]\(CHANGELOG\.md\) for the rest\._\n$/);
  }
  // A cut inside the new entries leaves no <details> to close.
  const many = old.map((l) => l.replace("Entry", "New entry"));
  const out = buildReleaseNotes({
    tag: "v14.5.0-rc.2",
    changelogText: CHANGELOG,
    groups: { Added: many, Changed: [], Fixed: ["- Old fix."] },
    delta: { since: "14.5.0-rc.1", groups: { Added: many, Changed: [], Fixed: [] } },
  });
  assert.ok(Buffer.byteLength(out, "utf8") <= MAX_BYTES);
  assert.doesNotMatch(out, /<\/?details>/);
  assert.match(out, /\n- New entry number \d+ with some padding text\.\n\n---\n\n_Release notes truncated/);
});

test("truncation closes every open level of nested <details> blocks", () => {
  const old = Array.from({ length: 4000 }, (_, i) => `- Entry number ${i} with some padding text.`);
  // A hand-written <details> block in [Unreleased] nests inside the whole list's.
  const nested = (inner) =>
    CHANGELOG.replace(
      "- Hand-written fix.\n",
      `- Hand-written fix.\n\n<details>\n<summary>Hand-written details</summary>\n\n${inner}\n\n</details>\n`,
    );
  const cases = [
    // The cut falls after the closed inner block: one level open.
    { text: nested("- Hand-written detail."), fixed: old, depth: 1 },
    // The cut falls inside the inner block: two levels open.
    { text: nested(old.join("\n")), fixed: ["- Old fix."], depth: 2 },
  ];
  for (const { text, fixed, depth } of cases) {
    for (let pad = 0; pad < 60; pad++) {
      const fresh = `- New${"x".repeat(pad)}.`;
      const out = buildReleaseNotes({
        tag: "v14.5.0-rc.2",
        changelogText: text,
        groups: { Added: [fresh], Changed: [], Fixed: fixed },
        delta: { since: "14.5.0-rc.1", groups: { Added: [fresh], Changed: [], Fixed: [] } },
      });
      const label = `depth ${depth}, pad ${pad}`;
      assert.ok(Buffer.byteLength(out, "utf8") <= MAX_BYTES, label);
      const lines = out.split("\n");
      const opens = lines.filter((l) => l === "<details>").length;
      assert.equal(opens, lines.filter((l) => l === "</details>").length, label);
      assert.ok(opens >= depth, label);
      const closers = "\n\n</details>".repeat(depth);
      assert.ok(out.endsWith(`${closers}\n\n---\n\n_Release notes truncated; see the [full changelog](CHANGELOG.md) for the rest._\n`), label);
    }
  }
});

test("without a stable release or hand-written entries, the delta names the last stable release generically", () => {
  const out = buildReleaseNotes({
    tag: "v1.0.0-rc.2",
    changelogText: "# Changelog\n\n## [Unreleased]\n",
    groups: { Added: ["- Old feature.", "- New feature."], Changed: [], Fixed: ["- Old fix."] },
    delta: { since: "1.0.0-rc.1", groups: { Added: ["- New feature."], Changed: [], Fixed: [] } },
  });
  assert.equal(
    out,
    [
      deltaIntro("1.0.0", "1.0.0-rc.1", "the last stable release"),
      "",
      "## New since 1.0.0-rc.1",
      "",
      "### Added",
      "",
      "- New feature.",
      "",
      "## Every change since the last stable release",
      "",
      "<details>",
      "<summary>Show all 3 changes</summary>",
      "",
      "### Added",
      "",
      "- Old feature.",
      "- New feature.",
      "",
      "### Fixed",
      "",
      "- Old fix.",
      "",
      "</details>",
      "",
    ].join("\n"),
  );
});

// Fixture repositories ignore the developer's own git config (hooks, signing, line endings).
function fixtureGit(cwd, home, ...args) {
  const r = spawnSync(
    "git",
    [
      "-c", "user.name=test", "-c", "user.email=test@example.invalid",
      "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false",
      ...args,
    ],
    { cwd, encoding: "utf8", env: fixtureEnv(home) },
  );
  assert.equal(r.status, 0, `git ${args.join(" ")}: ${r.stderr}`);
  return r;
}
const fixtureEnv = (home) => {
  const env = {
    ...process.env,
    GIT_CONFIG_GLOBAL: join(home, "gitconfig"),
    GIT_CONFIG_NOSYSTEM: "1",
    // Git never looks above the throwaway directory for a repository.
    GIT_CEILING_DIRECTORIES: home,
  };
  // Set by a git hook running these tests, they would point git at this repository.
  for (const name of ["GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR"]) delete env[name];
  return env;
};

test("the script compares fragment names with the previous pre-release's tag in its own repository", () => {
  const scriptsDir = dirname(fileURLToPath(import.meta.url));
  const home = mkdtempSync(join(tmpdir(), "plugy-changelog-extract-"));
  try {
    writeFileSync(join(home, "gitconfig"), "");
    const tree = join(home, "tree");
    mkdirSync(join(tree, "scripts"), { recursive: true });
    mkdirSync(join(tree, "changelog.d"));
    for (const f of ["changelog-extract.mjs", "changelog-lib.mjs", "version-lib.mjs"]) {
      copyFileSync(join(scriptsDir, f), join(tree, "scripts", f));
    }
    const changelogText = [
      "# Changelog",
      "",
      "## [Unreleased]",
      "",
      "## [0.9.0] - 2001-01-01",
      "",
      "### Added",
      "",
      "- Stable feature.",
      "",
      "[Unreleased]: https://example.invalid/compare/v0.9.0...HEAD",
      "",
    ].join("\n");
    writeFileSync(join(tree, "CHANGELOG.md"), changelogText);
    const fragment = (name, body) => writeFileSync(join(tree, "changelog.d", name), `${body}\n`);
    fragment("README.md", "# Not a fragment");
    fragment("added-kept.md", "- Kept feature.");
    fragment("changed-edited.md", "- Edited change.");
    fragment("fixed-renamed.md", "- Renamed fix.");
    fixtureGit(tree, home, "init", "-q");
    fixtureGit(tree, home, "add", "-A");
    fixtureGit(tree, home, "commit", "-q", "-m", "rc.1");
    fixtureGit(tree, home, "tag", "-a", "v1.0.0-rc.1", "-m", "v1.0.0-rc.1");
    fixtureGit(tree, home, "tag", "-a", "v0.9.0", "-m", "v0.9.0");
    fragment("added-brand-new.md", "- Brand new feature.");
    renameSync(join(tree, "changelog.d", "fixed-renamed.md"), join(tree, "changelog.d", "fixed-renamed-on-top.md"));
    fragment("fixed-renamed-on-top.md", "- Renamed fix, reworked.");
    fragment("changed-edited.md", "- Edited change, reworded.");
    fixtureGit(tree, home, "add", "-A");
    fixtureGit(tree, home, "commit", "-q", "-m", "rc.2");

    const run = (tag = "v1.0.0-rc.2") =>
      spawnSync(process.execPath, [join(tree, "scripts", "changelog-extract.mjs"), tag], {
        cwd: home,
        encoding: "utf8",
        env: fixtureEnv(home),
      });
    const { groups } = readFragments(join(tree, "changelog.d"));
    // No pre-release below rc.1 since 0.9.0: the whole list, and nothing to report.
    const first = run("v1.0.0-rc.1");
    assert.equal(first.status, 0, first.stderr);
    assert.equal(first.stderr, "");
    assert.equal(first.stdout, buildReleaseNotes({ tag: "v1.0.0-rc.1", changelogText, groups }));
    const r = run();
    assert.equal(r.status, 0, r.stderr);
    assert.equal(r.stderr, "");
    assert.equal(
      r.stdout,
      buildReleaseNotes({
        tag: "v1.0.0-rc.2",
        changelogText,
        groups,
        delta: {
          since: "1.0.0-rc.1",
          groups: { Added: ["- Brand new feature."], Changed: [], Fixed: ["- Renamed fix, reworked."] },
        },
      }),
    );
    assert.ok(r.stdout.startsWith(`${deltaIntro("1.0.0", "1.0.0-rc.1", "0.9.0")}\n\n## New since 1.0.0-rc.1\n\n`));
    const [fresh, all] = r.stdout.split("<details>");
    assert.match(fresh, /- Brand new feature\.\n[\s\S]*- Renamed fix, reworked\./);
    assert.doesNotMatch(fresh, /Edited change|Kept feature/);
    assert.match(all, /<summary>Show all 4 changes<\/summary>[\s\S]*- Edited change, reworded\./);

    // Without the history the notes are the whole list alone, and stderr says why.
    rmSync(join(tree, ".git"), { recursive: true, force: true });
    const plain = run();
    assert.equal(plain.status, 0, plain.stderr);
    assert.equal(plain.stdout, buildReleaseNotes({ tag: "v1.0.0-rc.2", changelogText, groups }));
    assert.doesNotMatch(plain.stdout, /<details>/);
    assert.equal(plain.stderr, "changelog-extract: git could not list tags; the notes list every entry.\n");
  } finally {
    rmSync(home, { recursive: true, force: true });
  }
});
