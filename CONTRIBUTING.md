# Contributing to GoldenEye Coop

GoldenEye Coop is an online co-op and versus edition of GoldenEye 007, built on the [Project Midas](https://github.com/murkantor/Project-Midas) PC port. It aims to play like the original game, with every change tested in game. Contributions are welcome when they meet that bar.

## Where things stand

- What is open in each release: the "Known issues" in the newest [release notes](coop/release-notes/).
- What has and hasn't been tested, mission by mission: [`coop/TEST-CHECKLIST.md`](coop/TEST-CHECKLIST.md).
- How co-op and versus work, and why: [`coop/NOTES.md`](coop/NOTES.md).

Open an issue before starting anything big, so effort isn't duplicated. Fixes that aren't specific to co-op or versus belong upstream in [Project Midas](https://github.com/murkantor/Project-Midas) too, so every player of the port gets them.

## What a good pull request looks like

1. **One focused change.** Fix one bug or add one feature per PR. Don't mix in unrelated cleanup.
2. **Builds on both platforms.** CI builds Linux and Windows; it must pass.
3. **Tested in game.** Say what you tested and on which level or menu. "It compiles" is not enough.
4. **Matches the code around it.** Keep the decompiled names. Keep comments short: what the code does and why, in at most two plain sentences.
5. **No game assets.** Never commit ROM data or extracted assets.

## Co-op and versus changes

- **Test with at least two players**: two PCs over Steam, or two copies on one Linux machine over the mock Steam with the scripts in [`coop/ci/checklist/`](coop/ci/checklist/). Say which side you tested (host, joiner, or both) and which of the two setups you used.
- **Say who needs the change.** Some fixes only help when the host has them, some when every player does. Release notes say which, so the PR should too.
- **Network changes are a shared contract.** Anything that changes what goes over the wire (packet layouts, hello or lobby fields) raises `COOP_VERSION` in `src/game/coop.c`, which stops older games from joining. Talk about it in an issue first: changes like that are grouped into one version bump.
- **Single player must not change** unless that is the point of the PR.

## AI-assisted contributions

AI tools are fine to use for code. Low-effort AI output is not.

- **Code only.** AI-generated images, textures, models, sound or music will never be accepted. Art and audio must be made by people.
- Say in the PR that AI was used (the PR template has a box for it).
- You must understand every line you submit and be able to answer questions about it.
- You must have run and tested the change yourself.
- Large unreviewed or untested AI changes will be closed without review.

## Forks

The code is MIT licensed, so you are free to fork it. If you do, please:

- give your fork its own name, not "GoldenEye Coop" or "Project Midas",
- link back to this repository,
- send fixes back here as pull requests so everyone benefits.

## License

By contributing you agree that your contribution is licensed under the MIT license in [LICENSE](LICENSE). That license covers the port code only; see its "Scope" section.
