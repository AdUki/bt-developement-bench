# Upstreaming fixes

A fix made on the bench (a commit on `bench` in `src/bluez`, `src/pipewire` or `src/linux`) goes
upstream as its own branch, cut from the upstream project and not from `bench`. `bench` carries
the image's pin and the bench's other commits, and upstream wants a series that applies to
its current tree. `tools/dev/upstream` (or the `make upstream-*` targets that wrap it) does the
mechanical part:

```bash
make upstream-start COMP=linux TOPIC=bis-cleanup FIX=1 PICK=9b9167299fc3,605667928274
#   → ~/projects/linux-bis-cleanup, branch bis-cleanup on bluetooth/master, the commits picked
make upstream-trailers DIR=~/projects/linux-bis-cleanup AI=1   # Claude lines out, Assisted-by in
make upstream-check DIR=~/projects/linux-bis-cleanup AI=1 BUILD=1 TEST=1
make upstream-export DIR=~/projects/linux-bis-cleanup SIGNOFF=1
#   → build/upstream/out/bis-cleanup-v1/: patches, cover letter, To/Cc set; then git send-email
```

The script never sends mail, pushes or opens a merge request. The person whose name is on the
series reads it and sends it: every project below asks that of contributions made with an AI
assistant, and the kernel's Signed-off-by is a legal statement that only a human can make.

## The flow

1. **`start <proj> <topic>`** makes a worktree of the bench's tree (`src/<proj>`, or `--from`) at
   `~/projects/<proj>-<topic>` (`UPSTREAM_WORKTREES`), on a branch `<topic>` cut from upstream.
   It adds the upstream remote if the tree has none. The kernel branches off bluetooth-next
   by default, or off bluetooth with `--fix`. `--pick` cherry-picks bench commits onto it. A shallow
   tree (`src/linux`) fetches only the base's tip, which is enough to cherry-pick and build on.
2. Rework the commits there until they meet the rules below. Tests for the fix go in the same
   series (BlueZ: unit tests, or a tester case in `tools/*-tester.c` for a kernel fix). Upstream
   reviewers ask for them.
3. **`trailers`** rewrites the branch: it drops the `Co-Authored-By`/`Claude-Session` lines Claude
   Code adds and BlueZ's Signed-off-by lines. With `--ai` it adds the project's Assisted-by
   tag. It refuses a branch that is pushed already.
4. **`check`** checks every commit from the base to HEAD. The base is the upstream ref HEAD is
   fewest commits ahead of, or `--base`.
   - **Messages:** the rules below.
   - **checkpatch:** run the way the Bluetooth CI runs it.
   - **PipeWire:** whitespace and spelling.
   - **`--build`:** builds each commit in a worktree of its own (a commit that adds a warning the
     base did not have fails), then runs the unit tests CI would pick.
   - **`--test`** (kernel): builds the tester kernel and runs the testers CI would pick, with
     `tools/test-runner` and the testers of `UPSTREAM_BLUEZ`.
5. **`export`** checks, then writes the series with `git format-patch` into
   `build/upstream/out/<branch>-v<N>/`, with the list in To and the maintainers in Cc. It adds a
   cover letter when there is more than one patch, and prints the `git send-email` command.
   - **`-v N`** for a new version: put what changed under each patch's `---` or in the cover letter.
   - **`--signoff`** adds your Signed-off-by (kernel only).
   - **PipeWire:** export prints the merge request steps instead.

Settings (environment or `bench.conf`, see `bench.conf.sample`):

| Setting | Meaning |
|---|---|
| `UPSTREAM_WORKTREES` | Where `start` puts worktrees (default `~/projects`) |
| `UPSTREAM_LINUX_HISTORY` | A full kernel clone, for checking Fixes: tags from a shallow tree |
| `UPSTREAM_BLUEZ` | The BlueZ tree for the testers and the CI kernel configs (`doc/ci.config`, `doc/tester.config`). Default: upstream master in a worktree of `src/bluez`, fetched once a day. The testers must not be older than the kernel: a test whose emulator support is missing hangs. |
| `UPSTREAM_ELL` | The ELL source for building BlueZ |
| `UPSTREAM_AI_MODEL` | The model named in BlueZ's Assisted-by |
| `UPSTREAM_CHECKPATCH` | A `checkpatch.pl` recent enough to know `Assisted-by:` (6.9's flags it) |
| `UPSTREAM_CACHE` | Build worktrees, kernels, testers and the CI config (default `~/.cache/btbench-upstream`). It must be under `$HOME`, where test-runner's VM can see it. Each checked tree has its own builds, logs (`run/<proj>-<hash>/`) and lock, so checks of different series can run at once. |
| `UPSTREAM_TESTERS` | Run these testers instead of the ones CI would pick (`"iso-tester mgmt-tester"`) |
| `UPSTREAM_TESTER_TIMEOUT` | Seconds before a hung tester VM is killed (default 1200) |

Which testers and unit tests run comes from the Bluetooth CI's own file mapping
(`config.json` of github.com/bluez/action-ci, fetched into the cache once a day), so it changes
when CI's does. gitlint runs with the CI's config when it is installed (`pipx install gitlint`).
Without it the script checks the same limits itself.

## Linux Bluetooth

| | |
|---|---|
| List | linux-bluetooth@vger.kernel.org; Cc the maintainers (`scripts/get_maintainer.pl`): Marcel Holtmann, Luiz Augusto von Dentz |
| Base | Fixes: `bluetooth.git` master (goes to `net`). Features: `bluetooth-next.git` master (goes to `net-next`). CI applies every kernel series to bluetooth-next, so a fix must apply there too. |
| Subject | `[PATCH vN M/N] Bluetooth: <component>: <Summary>`, for example `Bluetooth: hci_sync: ...`. CI's SubjectPrefix check fails without `Bluetooth: `. At most 75 characters (submitting-patches.rst); CI's gitlint fails above 80. |
| Body | Not empty. Lines at most 75 (checkpatch), except tags and URLs. It says what was broken, how it shows, and why the change fixes it. |
| Fixes: | `Fixes: <12+ hex> ("<exact subject>")`, in the trailer block (no blank lines on both sides), naming a commit that exists and is an ancestor (CI VerifyFixes). Add `Cc: stable@vger.kernel.org` when it should be backported. |
| Signed-off-by | Required, matching the author and the committer (CI VerifySignedoff). Only a human adds one. |
| AI | `Assisted-by: LLM [tools]`, for example `Assisted-by: LLM coccinelle` (`Documentation/process/coding-assistants.rst`, current form since 816d9992d9ed4). An assistant must not add Signed-off-by, and `Co-developed-by` is for people only. `generated-content.rst`: say in the changelog or cover letter which parts were assisted and how they were tested. |
| Versions | `-v2`, with the changes since v1 below `---`. `[PATCH RESEND]` for an unchanged resend. |

CI runs, per series:
- **Checks:** checkpatch `--ignore UNKNOWN_COMMIT_ID`, gitlint with `contrib-body-requires-signed-off-by`, VerifyFixes and VerifySignedoff.
- **Builds:** `net/bluetooth/` and `drivers/bluetooth/` with BlueZ's `doc/ci.config`, plain, `W=1`, `C=1` (sparse), with `LLVM=1` and 32-bit, and incrementally per patch.
- **TestRunner:** the testers mapped to the changed files (`mgmt.c` → mgmt-tester and mesh-tester, `iso.c` → iso-tester, `hci_*.c` → all of them, `drivers/bluetooth/` → none).

`check --build --test` covers the checks, the `W=1` per-patch build, sparse when it is installed,
and the testers. It doesn't cover the LLVM and 32-bit builds.

## BlueZ

| | |
|---|---|
| List | linux-bluetooth@vger.kernel.org (`.b4-config` sets the same). GitHub pull requests on bluez/bluez are closed with a pointer to the list. |
| Base | `bluez.git` master |
| Subject | `[PATCH BlueZ vN M/N] <component>: <Summary>`. The component follows the files' history (`shared/bap`, `bap`, `transport`, `emulator`, `iso-tester`). At most 72 characters (`.gitlint`, maintainer-guidelines); CI fails above 80. `start` sets `format.subjectprefix`. |
| Body | Not empty. Lines at most 72, and CI fails above 80. |
| Split | One top-level directory per patch: `doc/`, `lib/`, `src/`, `tools/` each get their own, and every step builds (HACKING rule 3). Fixes go before features. |
| Signed-off-by | **None.** "Including them is actually an error" (HACKING, maintainer-guidelines). `trailers` removes them. |
| Fixes: | Same form as the kernel's. |
| AI | `Assisted-by: AGENT:MODEL [tools]`, for example `Assisted-by: Claude:claude-opus-5-5` (BlueZ's `AGENTS.md`, which still uses the kernel's older form). `make check` and checkpatch must pass, and say what could not be tested. The assistant never sends anything. |

CI runs, per series:
- **Checks:** checkpatch with BlueZ's `.checkpatch.conf`, and gitlint.
- **Builds:** ELL and BlueZ (`bootstrap-configure`: maintainer mode, so `-Werror`), incrementally per patch.
- **Tests:** `make check` restricted to the unit tests mapped to the changed files, distcheck, valgrind, scan-build, smatch, an external-ELL build, and the functional tests.

`check --build` covers the checks, the per-patch build and the mapped `make check`. It doesn't
cover distcheck, valgrind, scan-build, smatch or the functional tests (`test/test-functional`).

## PipeWire

| | |
|---|---|
| Where | A merge request on gitlab.freedesktop.org/pipewire/pipewire, from a fork there. A new account asks for fork rights first, with a "User verification" issue in freedesktop/freedesktop. MR pipelines run in the parent project, and a maintainer may have to start them. |
| Base | `master` |
| Subject | `<module>: [<sub>: ]<summary>` in lowercase, as in history (`bluez5: media-source: ...`). Reference issues as `Fixes #1234`. |
| Signed-off-by | Not required |
| Style | Tabs at width 8, no trailing whitespace (`.editorconfig`). CI's spellcheck job rejects "Pipewire". |
| AI | `AGENTS.md` (since 2026-09-21): no AI-written commit messages or MR descriptions, no AI replies to reviewers, no `Co-authored-by` or similar trailers, and no agent writes to GitLab. You must be able to explain the change. So for PipeWire the script checks and builds but does not touch messages beyond removing Claude's trailers, and `export` only prints the steps. |

CI runs gcc and clang builds with several option sets, `meson test`, ASan/UBSan, valgrind,
spellcheck, shellcheck and the doc and header checks. A `bluez_tests` job runs too: functional
tests in a VM with a WirePlumber session. `check --build` covers a build per commit with
`-Dbluez5=enabled` (so a missing dependency fails the setup instead of skipping the code),
`meson test`, the whitespace and spelling checks, and shellcheck on changed scripts.
