// PR-context guard: the PR workflow writes the BASE...HEAD changed-path list
// and the head commit's path list to files, then runs
//   node scripts/check-changelog-fragment.mjs <changed.txt> <present.txt> <title>
// which fails the check when shipped code changed without a changelog.d fragment.
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { FRAGMENT_RE } from "./changelog-lib.mjs";

// The directories whose changes reach players: every program's source (src/),
// the files installed beside them (game-files/), the installer (installer/) and
// the vendored libraries compiled into them (third_party/).
export const SHIPPED_DIR_RE = /^(src|game-files|installer|third_party)\//;

export function fragmentVerdict({ changedFiles, presentFiles, prTitle }) {
  if (
    ![changedFiles, presentFiles].every(
      (files) =>
        Array.isArray(files) && files.every((file) => typeof file === "string"),
    ) ||
    typeof prTitle !== "string"
  ) {
    throw new TypeError("Expected two string arrays and a PR title string.");
  }
  // Empty diffs are an explicit skip, just like changes outside the shipped dirs.
  if (!changedFiles.some((file) => SHIPPED_DIR_RE.test(file))) {
    return {
      required: false,
      satisfied: true,
      reason:
        "No src/, game-files/, installer/ or third_party/ changes; fragment not required.",
    };
  }
  // Stay in sync with changelog-draft.mjs through its shared FRAGMENT_RE.
  // Its readdirSync scan is top-level and directory-case-sensitive.
  if (
    presentFiles.some(
      (file) =>
        file.startsWith("changelog.d/") &&
        !file.slice("changelog.d/".length).includes("/") &&
        FRAGMENT_RE.test(file.slice("changelog.d/".length)),
    )
  ) {
    return {
      required: true,
      satisfied: true,
      reason: "Changelog fragment present, thanks!",
    };
  }
  if (/skip-changelog/i.test(prTitle)) {
    return {
      required: false,
      satisfied: true,
      reason: "'skip-changelog' in the PR title; fragment not required.",
    };
  }
  return {
    required: true,
    satisfied: false,
    reason: [
      "::error::This PR changes src/, game-files/, installer/ or third_party/ but adds no changelog.d/ fragment.",
      "A fragment DELETED by this PR doesn't count; only fragments present on the head commit do.",
      "Add changelog.d/<added|changed|fixed>-<slug>.md (see changelog.d/README.md),",
      "or add the 'no-changelog' label / put 'skip-changelog' in the PR title.",
    ].join("\n"),
  };
}

function main() {
  try {
    const [changedPath, presentPath, prTitle] = process.argv.slice(2);
    const readList = (path) => {
      const text = readFileSync(path, "utf8");
      if (text.includes("\0")) throw new Error("NUL byte in path list.");
      // CRLF-tolerant: a Windows-written list must not leave `\r` on the paths.
      return text.split(/\r?\n/).filter(Boolean);
    };
    const verdict = fragmentVerdict({
      changedFiles: readList(changedPath),
      presentFiles: readList(presentPath),
      prTitle,
    });
    console.log(verdict.reason);
    if (!verdict.satisfied) process.exitCode = 1;
  } catch (error) {
    console.error(`::error::changelog-fragment: ${error.message}`);
    process.exitCode = 1;
  }
}

if (
  process.argv[1] &&
  resolve(process.argv[1]) === fileURLToPath(import.meta.url)
) {
  main();
}
