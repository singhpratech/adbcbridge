<!-- SPDX-License-Identifier: Apache-2.0 -->
# Security and release verification

## Supported versions

| Version | Supported |
|---|---|
| 0.1.x (current release) | Yes: security fixes ship as a patch release |
| `main` | Yes: fixes land here first |
| Anything older than the current minor | No |

A security fix produces a new release on every registry the project publishes to
(PyPI, crates.io, nuget.org, Maven Central, the Go module and the GitHub Release
binaries) within the same release.

## Reporting a vulnerability

Please do not open a public issue for a security problem. Use GitHub's private
vulnerability reporting for this repository:

<https://github.com/singhpratech/adbcbridge/security/advisories/new>

You will get an acknowledgement within three business days. Confirmed problems get
a fix or a documented mitigation, coordinated with you, with a target of thirty days
from confirmation; the advisory is published with the release that carries the fix,
and you are credited unless you ask not to be.

**In scope:** the C driver library (`src/`), the Python, Rust, C#, Java, Go and R
bindings, `install.sh`, the release workflow and the published packages.

**Out of scope, but still wanted:** defects in third-party ODBC drivers or database
servers that the compatibility work turns up. Those go to the project concerned;
[Upstream](../UPSTREAM.md) is the public record of what has been found
and reported, and a note through the same private channel is welcome if a finding
has a security angle.

## Verifying a download

Every GitHub Release, v0.1.0 included, carries `SHA256SUMS`, a checksum of each
asset, and `SHA256SUMS.asc`, a detached signature of that file by the project's
release key (fingerprint `1515 33D6 5DDE 6F9D AA04 558C 95CC 478E 1985 A908`, on
`keyserver.ubuntu.com`; the same key signs the Maven Central artifacts):

```sh
gpg --keyserver keyserver.ubuntu.com --recv-keys 95CC478E1985A908
gpg --verify SHA256SUMS.asc SHA256SUMS
sha256sum -c SHA256SUMS --ignore-missing
```

Releases after v0.1.0 also carry:

- a build-provenance attestation for every asset, recorded by GitHub's Sigstore
  instance, which ties the file to the exact workflow run and commit that built it:
  ```sh
  gh attestation verify adbcbridge-v0.2.0-linux-x64.tar.gz --repo singhpratech/adbcbridge
  ```
- `adbcbridge-<tag>.spdx.json`, a software bill of materials of the source tree the
  release was built from (SPDX 2.3 JSON).

Package registries verify their own uploads: PyPI and nuget.org publications use
trusted publishing from this repository's workflows, and Maven Central checks the GPG
signature above on every file.

## Support window

The current minor release is supported: bug fixes and security fixes ship as patch
releases of it, on every registry at once, and `main` always carries them first.
When a new minor is released the previous one stops receiving fixes; there is no
long-term-support line at this stage of the project. Questions and bug reports go
through [GitHub issues](https://github.com/singhpratech/adbcbridge/issues); the
[FAQ](faq.md#how-do-i-report-a-bug-and-what-should-i-include) says what a useful
report contains.

## Dependencies

Dependabot watches the Python, Rust, NuGet, Maven and Go manifests and the GitHub
Actions this repository uses, and opens pull requests for security updates. The
driver library itself depends only on a C11 compiler and an ODBC driver manager.
