# Contributing / commit rules

Repository: `xiexgxgxgxg/android_kernel_samsung_veyronltectc`
Main branch: `lineage-18.1`

These rules are mandatory for every commit in this repository.

## 1. Identity

Every commit must carry the repository owner's identity for **both** the
author and the committer:

```
想桃子吃的屑 <xiangtaozichidexie@gmail.com>
```

`user.name` / `user.email` are set in the repository config, so a plain
`git commit` is already correct.  When scripting commits, do not pass
`-c user.name=...` with a placeholder, and verify afterwards:

```sh
git log -1 --format='%an <%ae> | %cn <%ce>'
```

Rewriting a pushed commit's identity requires `git push --force-with-lease`;
tell the owner before doing that.

## 2. Language

Commit messages are written in **English**.  Code comments and documentation
inside the tree are English as well; discussion with the owner may be in
Chinese.

## 3. Format

```
<area>: <imperative summary>

<why the change is needed, then what it does>

<optional detail: bullet list, tables, before/after numbers>
```

* Subject line: `area: summary`, **at most 72 characters**, imperative mood,
  no trailing period.  `area` is the subsystem or file group, e.g.
  `arch/arm64:`, `drivers/mmc:`, `net:`, `3.18.140:`, `chore:`.
* One blank line after the subject, then the body.
* Body wrapped at 72 columns.  Explain *why* first; the diff already shows
  *what*.
* Use `-` for bullet lists.  Blank line before and after a list.
* No `Signed-off-by` trailer is required, but do not add fake ones.
* Reference decisions and trade-offs that are not obvious from the code, and
  mention any deliberate feature that was dropped.

## 4. Scope

One commit = one logical change.  Split mechanical work (a merge, a rename)
from behaviour changes (a fix, a workaround) so either can be reverted alone.

## 5. What must never be committed

Build output.  It is ignored by `.gitignore` and untracked; see
`chore: stop tracking kernel build output`.  This includes `vmlinux`,
`vmlinux.o`, `*.o`, `built-in.o`, `Module.symvers`, `include/config/`,
`include/generated/` and the generated lexer/parser sources.

The top-level `.config` is **not** tracked.  `arch/arm64/configs/veyronlte_ctc_defconfig`
is the configuration source of truth.  An out-of-tree build (`O=`, which the
LineageOS build system uses) refuses to run when `$(srctree)/.config` exists,
and a tracked `.config` silently goes stale when the defconfig changes.

## 6. Shell hygiene when scripting commits

Write commit messages with a here-document and `git commit -F <file>`, not
with `-m "..."`.  A backtick or `$` inside a double-quoted `-m` argument is
expanded by the shell and has already corrupted one commit message and
triggered an unintended `make`.
