<!-- SPDX-License-Identifier: Apache-2.0 -->
# Releasing adbcBridge

One version number, spelled out in one place and copied everywhere else by a script; one
tag, which CI refuses unless it names that version; then the registries, one at a time.

## 1. Bump

```sh
python3 scripts/bump_version.py --set 0.1.1
python3 scripts/bump_version.py --check
git switch -c release/0.1.1 && git commit -am "release: 0.1.1" && git push -u origin HEAD
```

`--set` rewrites every manifest (`CMakeLists.txt`, the Python, Rust, C#, Java packages,
the driver's own version string) and every install snippet in the docs. `--check` is what
CI runs on every push and pull request, and what the Release workflow runs against the
tag before it builds anything. Open the pull request, let CI go green, merge.

## 2. Tag

```sh
git switch main && git pull --ff-only
git tag -a v0.1.1 -m "adbcBridge 0.1.1" && git push origin v0.1.1
git tag -a go/v0.1.1 -m "adbcBridge Go module 0.1.1" && git push origin go/v0.1.1
```

The `v*` tag starts `.github/workflows/release.yml`: it checks the tag against the
manifests, builds the libraries for Linux x86_64 and aarch64, macOS arm64, Windows x64 and
Win32, the wheels, crate, nupkg and jar, and attaches them with `SHA256SUMS`, the signature,
provenance and SBOM. The `go/v*` tag is what the Go module proxy resolves; nothing builds
for it.

## 3. Publish

Each registry is a separate, deliberate step; none happens on its own.

| Registry | How |
|---|---|
| PyPI | Actions → *Publish to PyPI* → Run workflow with the tag (trusted publishing). |
| nuget.org | Actions → *Publish to NuGet* → Run workflow with the tag and the nuget.org user that owns the trusted-publishing policy. |
| Maven Central | Actions → *Publish to Maven Central* → Run workflow with the tag; then, in the Central Portal, review the staged deployment and click **Publish**. `java/PUBLISHING.md` has the token and key details. |
| crates.io | From a checkout of the tag: `cd rust && cargo publish` (`cargo login` first). |
| Go | Nothing to do beyond the `go/v*` tag; `go list -m github.com/singhpratech/adbcbridge/go@v0.1.1` confirms the proxy sees it. |

Then confirm each one shows the new version before touching anything public:

```sh
curl -s https://pypi.org/pypi/adbcbridge/json | python3 -c 'import json,sys; print(json.load(sys.stdin)["info"]["version"])'
curl -s https://crates.io/api/v1/crates/adbcbridge | python3 -c 'import json,sys; print(json.load(sys.stdin)["crate"]["max_version"])'
curl -s 'https://azuresearch-usnc.nuget.org/query?q=packageid:adbcbridge' | python3 -c 'import json,sys; print(json.load(sys.stdin)["data"][0]["version"])'
curl -s https://repo1.maven.org/maven2/org/adbcbridge/adbcbridge/maven-metadata.xml | grep -o '<latest>[^<]*'
```

## 4. Site and docs

The site is generated from the docs, so after the bump merges, regenerate and push
`gh-pages` (the docs page, landing, matrix and bench pages carry the version). Notes that
promised a fix "in 0.1.1" get their one-line "released" update now, not before.

## 5. Announce

Only after every registry above shows the new version. The release notes GitHub generated
from the pull-request titles are the changelog; edit them on the release page if a title
needs a plainer sentence.
