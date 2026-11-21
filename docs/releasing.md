# Releasing

Two long-lived branches (#122):

- **`dev`** is where work happens. Every change reaches it by a pull request,
  and the four CI checks must pass first. `dev` can be ahead of what has been
  tested on hardware.
- **`main`** holds only firmware that has passed the bench plan (#70) on real
  hardware. A car is flashed only from a release on `main`, never from `dev`
  or a local build.

Both are protected by GitHub rulesets ("Protect dev", "Protect main"): no
direct push, no force-push, no deletion, one approval. Repo admins can merge
their own pull requests without an approval.

## Making a release

1. Pick the `dev` commit to release. To keep merging into `dev` while it is
   tested, branch it as `release/vX.Y`.
2. Flash that commit's images and run the bench plan. A failure is fixed on
   `dev` (or on the release branch, then merged back into `dev`), and the
   bench is run again.
3. Open a pull request into `main`. In it, record:
   - the commit tested,
   - the boards (and their roles) it was tested on,
   - the bench cases that passed, and any that were skipped and why.
4. Merge it, and tag the merge `vX.Y.Z`.
5. Make a GitHub Release for the tag with both images attached, renamed,
   since both build as `tim.elf`:
   `tim-vcu-vX.Y.Z.elf` and `tim-bms-vX.Y.Z.elf`, built with the Release
   preset.

Version numbers: raise X for a change in how the car is driven or charged or
in the CAN frames other modules read, raise Y for new features, raise Z for
fixes.

## Hotfix

A fix the car needs before the next release branches from `main`, is bench
checked, goes into `main` by a pull request and is tagged (raise Z). Then
merge `main` back into `dev` so the fix is not lost.

## First release

`main` currently isn't part of `dev`'s history: it has one stray commit
(`7a909c5`, a `.gdbinit`). For the first release, point `main` straight at the
tested commit instead of merging into it. Turn "Protect main" off for that one
move, then back on.
