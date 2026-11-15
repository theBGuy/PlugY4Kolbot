// fragmentVerdict: which PR diffs need a changelog.d fragment, and what
// satisfies or exempts them. Pure path lists; no git involved.
import assert from "node:assert/strict";
import { test } from "node:test";

import { fragmentVerdict } from "./check-changelog-fragment.mjs";

const verdict = (changedFiles, presentFiles = [], prTitle = "feat: x") =>
  fragmentVerdict({ changedFiles, presentFiles, prTitle });

test("changes outside the shipped directories need no fragment", () => {
  for (const changed of [[], ["README.md"], ["scripts/release.mjs"], [".github/workflows/x.yml"], ["tests/UpdaterTests.cpp"], ["srcish/x.cpp"], ["third_party.md"]]) {
    const v = verdict(changed);
    assert.equal(v.required, false, JSON.stringify(changed));
    assert.equal(v.satisfied, true);
  }
});

test("each shipped directory requires a fragment", () => {
  for (const dir of ["src/plugy", "src/common", "src/launcher", "src/patcher", "src/updater", "game-files", "installer", "third_party/sqlite"]) {
    const v = verdict([`${dir}/file.cpp`]);
    assert.equal(v.required, true, dir);
    assert.equal(v.satisfied, false, dir);
    assert.match(v.reason, /::error::/);
    // Both escape hatches the workflow honors are named.
    assert.match(v.reason, /'no-changelog' label/);
    assert.match(v.reason, /'skip-changelog' in the PR title/);
  }
});

test("a top-level fragment on the head commit satisfies it", () => {
  assert.equal(verdict(["src/plugy/a.cpp"], ["changelog.d/fixed-thing.md"]).satisfied, true);
  // Nested, misnamed, README and wrong-case directory entries don't count.
  for (const present of [
    "changelog.d/sub/fixed-thing.md",
    "changelog.d/fix-thing.md",
    "changelog.d/README.md",
    "Changelog.d/fixed-thing.md",
  ]) {
    assert.equal(verdict(["src/plugy/a.cpp"], [present]).satisfied, false, present);
  }
});

test("skip-changelog in the title exempts", () => {
  const v = verdict(["src/common/x.h"], [], "refactor: tidy (skip-changelog)");
  assert.equal(v.required, false);
  assert.equal(v.satisfied, true);
});

test("malformed input throws", () => {
  assert.throws(() => fragmentVerdict({ changedFiles: "x", presentFiles: [], prTitle: "" }), TypeError);
  assert.throws(() => fragmentVerdict({ changedFiles: [], presentFiles: [], prTitle: 1 }), TypeError);
});
