# LogicTracer: versioning

Follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html). The root `VERSION` is the
single product version for the host tools and firmware. Use `MAJOR.MINOR.PATCH` in `VERSION`; the `v`
prefix belongs only in Git tags (e.g. `v0.8.1`).

- Before packaging or installing a changed build, compare `VERSION` and `CHANGELOG.md` with the last
  release **and** the version installed at the target. Never deliver different product behavior under
  an already installed or published version, even for a local GUI reinstall.
- Choose the highest bump required by all changes since that version: **PATCH** for compatible fixes,
  **MINOR** for compatible features or deprecations, **MAJOR** for incompatible public changes from
  `1.0.0` onward. Public interfaces include CLI options, output and exit codes, MCP/session contracts,
  capture formats and the firmware protocol. While on `0.y.z`, use a minor bump for features or breaking
  changes and call out incompatibilities in the changelog. Reset lower components when bumping.
- Update `VERSION` and the matching `CHANGELOG.md` entry together before building. Group changes that
  have not yet been delivered; do not bump again for the same changes, an identical reinstall, or a
  documentation-only edit.
- After installation, verify `--version` on the **installed** executable, not the build-tree copy,
  and report the old and new versions. Never move a release tag or replace a published release with
  changed contents; a version bump alone is not authorization to tag or publish.
