# Getting the co-op mission runs to run in GitHub Actions

`coop-missions.yml` plays every story mission with two copies of the game
(`coop/ci/mission_runs.sh`). That needs the GoldenEye 007 (USA) cart image,
`baserom.u.z64`, SHA-1 `abe01e4aeb033b6c0836819f549c791b26cfde83`. The
image is never stored in this repository.

Until a cart image reaches CI, the workflow's `missions` job is skipped and
its summary reads **NOT RUN**. On `main`, `coop/story` and `v*` tags the
`cart image` job also fails, so those runs are red. That is deliberate:
before 2026-11-01 the job reported green without running anything, and all
225 runs up to then were that empty skip.

This repository is **public**. Anything attached to one of its releases,
including a prerelease and including an encrypted file, can be downloaded
by anyone. Choose how CI gets the image (task board BUILD-2, risk register
OD-3) before setting anything up.

## Options

| Option | What you set up | Nothing public? | Workflow change |
| --- | --- | --- | --- |
| A. Private companion repo | A private repo holding `baserom.u.z64.gpg`, a read-only fine-grained token for it, `CI_ROM_KEY` | yes | yes: fetch from that repo instead of the `ci-rom` release |
| B. Self-hosted runner | A runner on your own machine with your own cart on its disk | yes | yes: `runs-on` your runner's label, read the cart from a local path |
| C. Local gate | Nothing in GitHub. Each PR that touches co-op code attaches a local `mission_runs.sh` summary | yes | none (the job stays NOT RUN, and red on main) |
| D. `ci-rom` release on this repo (what the workflow reads today) | `baserom.u.z64.gpg` on a public release, `CI_ROM_KEY` | **no**: the encrypted file is public | none |

Notes:

- **A** keeps GitHub's own runners and makes no public copy. The token
  should be fine-grained and limited to that one repo with read-only
  contents, stored as a secret here.
- **B**: GitHub advises against self-hosted runners on public repos,
  because code from a pull request can run on them. This workflow runs on
  `push` and manual runs only, not on `pull_request`, so only people with
  write access trigger it. Keep it that way if you choose B.
- **C** is the cheapest choice but it is not an automated gate. It relies on
  every PR posting its log, and review checking that it did.
- **D** is the setup the workflow was first written for. It is listed for
  completeness only. The review thread recommends against it.

After you pick one, the BUILD thread makes the workflow change (for A or B)
and confirms a real run with every mission executed.

## Steps for D (the current workflow, if you accept a public encrypted file)

Run these on your own machine with the cart image beside you:

    sha1sum baserom.u.z64      # must print abe01e4aeb033b6c0836819f549c791b26cfde83
    gpg --symmetric --cipher-algo AES256 -o baserom.u.z64.gpg baserom.u.z64
    gh release create ci-rom baserom.u.z64.gpg --prerelease --repo reaffirmed/goldeneye-coop
    gh secret set CI_ROM_KEY --repo reaffirmed/goldeneye-coop   # paste the gpg passphrase

Use a long random passphrase, since anyone can download the encrypted file
and try to guess it. Then start the workflow by hand ("Co-op mission runs",
Run workflow) and check that the `missions` job takes minutes, not seconds,
and that its summary lists all 20 stages.

## Running the missions locally (any option)

On Linux with the i686 build from `main.yml`, put the image in
`build_pc/Data/baserom.u.z64`, then run:

    mkdir -p build_pc/mock
    gcc -m32 -shared -fPIC -O1 -o build_pc/mock/libsteam_api.so coop/mock/steam_mock.c
    PAR=$(nproc) coop/ci/mission_runs.sh build_pc mission-runs

`mission-runs/summary.txt` has one PASS or FAIL line per stage. A stage that
never reports a result counts as FAIL.
