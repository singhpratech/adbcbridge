#!/usr/bin/env python3
# Copyright 2026 the adbcbridge authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
"""One version, everywhere it is spelled out.

The source of truth is ``project(... VERSION x.y.z)`` in CMakeLists.txt.  Every other
place that carries the release number -- the language packages' manifests, the driver's
own version string, the install snippets in the docs -- is listed here, so a release is
one command and CI can prove nothing was missed.

    scripts/bump_version.py --check            every listed file agrees with CMakeLists.txt
    scripts/bump_version.py --check --tag v0.1.1   ... and the tag names the same version
    scripts/bump_version.py --set 0.1.1        rewrite every listed file to 0.1.1

Two kinds of file.  MANIFESTS carry the version in one known place, matched by a
pattern; a manifest whose pattern does not match at all is an error (the file changed
shape and this script needs updating).  DOCS carry it in install commands and version
tables, where every bare occurrence of the current version is the release number; prose
that records history (ROADMAP.md, the FAQ, COMPATIBILITY.md, the benchmark files) is
deliberately not listed and keeps its dates and numbers.

Not touched on purpose: tests/ and bench/ have manifests of their own (smoke-test and
benchmark crates and poms) whose version is unrelated to the driver's.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ("CMakeLists.txt", r"(\bVERSION )(\d+\.\d+\.\d+)")

# (path, pattern) -- group 1 is the text before the version, group 2 the version.
MANIFESTS = [
    SOURCE,
    ("python/pyproject.toml", r'(^version = ")(\d+\.\d+\.\d+)'),
    ("python/src/adbcbridge/__init__.py", r'(^__version__ = ")(\d+\.\d+\.\d+)'),
    ("python/tests/test_package.py", r'(replace\("@PROJECT_VERSION@", ")(\d+\.\d+\.\d+)'),
    ("rust/Cargo.toml", r'(^version = ")(\d+\.\d+\.\d+)'),
    ("rust/Cargo.lock", r'(^name = "adbcbridge"\nversion = ")(\d+\.\d+\.\d+)'),
    ("src/odbc_internal.h", r'(#define ADBC_ODBC_DRIVER_VERSION ")(\d+\.\d+\.\d+)'),
    ("rust/csrc/src/odbc_internal.h", r'(#define ADBC_ODBC_DRIVER_VERSION ")(\d+\.\d+\.\d+)'),
    ("csharp/AdbcBridge/AdbcBridge.csproj", r"(<Version>)(\d+\.\d+\.\d+)"),
    # The project's own <version>, which is the first one in the file (the
    # dependency versions below it are properties, ${adbc.version} and friends).
    ("java/pom.xml", r"(<artifactId>adbcbridge</artifactId>\n  <version>)(\d+\.\d+\.\d+)"),
]

# Files where every bare occurrence of the current version means the release.
DOCS = [
    "README.md",
    "java/README.md",
    "docs/index.md",
    "docs/languages/python.md",
    "docs/languages/rust.md",
    "docs/languages/csharp.md",
    "docs/languages/java.md",
    "docs/getting-started/install-linux.md",
    "docs/getting-started/install-macos.md",
    "docs/getting-started/install-windows.md",
]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def source_version():
    m = re.search(SOURCE[1], read(SOURCE[0]), re.M)
    if not m:
        sys.exit(f"{SOURCE[0]}: no project VERSION found")
    return m.group(2)


def check(tag=None):
    want = source_version()
    problems = []
    for path, pattern in MANIFESTS:
        found = [m.group(2) for m in re.finditer(pattern, read(path), re.M)]
        if not found:
            problems.append(f"{path}: pattern matched nothing (file changed shape?)")
        for v in found:
            if v != want:
                problems.append(f"{path}: {v} (CMakeLists.txt says {want})")
    bare = re.compile(r"(?<![\d.])\d+\.\d+\.\d+(?![\d.])")
    for path in DOCS:
        # Anything that looks like an adbcbridge release number and is not the current one.
        for m in bare.finditer(read(path)):
            v = m.group(0)
            if v != want and looks_like_ours(read(path), m.start()):
                line = read(path).count("\n", 0, m.start()) + 1
                problems.append(f"{path}:{line}: {v} (CMakeLists.txt says {want})")
    if tag is not None:
        if tag.lstrip("v") != want:
            problems.append(f"tag {tag} does not name {want}")
    if problems:
        print("\n".join(problems))
        return 1
    print(f"all {len(MANIFESTS)} manifests and {len(DOCS)} docs agree: {want}")
    return 0


def looks_like_ours(text, pos):
    """Is the version at `pos` adbcbridge's own, not some dependency's?

    The docs also quote other projects' versions (Apache.Arrow.Adbc 0.24.0, arrow
    19.0.0, Maven plugins ...).  Ours sit on a line that names the package -- a wheel,
    jar or nupkg file name, ``adbcbridge = "..."``, ``AdbcBridge --version`` -- or on
    the line right after ``<artifactId>adbcbridge</artifactId>`` in a Maven snippet.
    """
    start = text.rfind("\n", 0, pos) + 1
    prev_start = text.rfind("\n", 0, max(start - 1, 0)) + 1
    end = text.find("\n", pos)
    line = text[start:end if end != -1 else len(text)].lower()
    prev = text[prev_start:start].lower()
    if "adbcbridge" in line:
        return True
    return "<artifactid>adbcbridge</artifactid>" in prev and "<version>" in line


def set_version(new):
    if not re.fullmatch(r"\d+\.\d+\.\d+", new):
        sys.exit(f"not a version: {new}")
    old = source_version()
    if old == new:
        sys.exit(f"already {new}")
    changed = []
    for path, pattern in MANIFESTS:
        text = read(path)
        out, n = re.subn(pattern, lambda m: m.group(1) + new, text, flags=re.M)
        if n == 0:
            sys.exit(f"{path}: pattern matched nothing; not bumping anything")
        if out != text:
            (ROOT / path).write_text(out, encoding="utf-8")
            changed.append(f"{path} ({n})")
    bare = re.compile(r"(?<![\d.])" + re.escape(old) + r"(?![\d.])")
    for path in DOCS:
        text = read(path)
        out = bare.sub(new, text)
        n = len(bare.findall(text))
        if n:
            (ROOT / path).write_text(out, encoding="utf-8")
            changed.append(f"{path} ({n})")
    print(f"{old} -> {new}:\n  " + "\n  ".join(changed))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true", help="verify every listed file agrees with CMakeLists.txt")
    g.add_argument("--set", metavar="X.Y.Z", help="rewrite every listed file to this version")
    ap.add_argument("--tag", help="with --check: the git tag that must name the same version (v prefix allowed)")
    a = ap.parse_args()
    if a.check:
        sys.exit(check(a.tag))
    sys.exit(set_version(a.set))


if __name__ == "__main__":
    main()
