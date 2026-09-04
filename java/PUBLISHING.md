<!-- SPDX-License-Identifier: Apache-2.0 -->
# Publishing the Java library to Maven Central

The library is published through the [Central Portal](https://central.sonatype.com)
by the **Publish to Maven Central** workflow (`.github/workflows/publish-maven.yml`).
It rebuilds the jar from the release tag with that release's native libraries, signs
every artifact with GPG, uploads the bundle with `central-publishing-maven-plugin`
(the `release` profile in `pom.xml`) and, by default, leaves the deployment staged
for a human to publish or drop.

## One-time setup

1. **Namespace.** `org.adbcbridge` is registered on the Central Portal and verified
   through a TXT record on `adbcbridge.org`. Namespaces are tied to the sign-in
   method used to register them; sign in the same way every time.
2. **User token.** Central Portal → account menu → *Generate User Token*. Store the
   two values as repository secrets `CENTRAL_USERNAME` and `CENTRAL_PASSWORD`
   (`gh secret set NAME` prompts for the value without echoing it).
3. **Signing key.** Central requires a GPG signature on every artifact, and verifies
   it against a public key server. On a machine you trust:

   ```sh
   gpg --quick-generate-key "adbcBridge release signing <release@adbcbridge.org>" ed25519 sign 2y
   gpg --list-secret-keys --keyid-format long          # note the key id after ed25519/
   gpg --keyserver keyserver.ubuntu.com --send-keys <KEYID>
   gpg --armor --export-secret-keys <KEYID> | gh secret set GPG_PRIVATE_KEY
   gh secret set GPG_PASSPHRASE                         # the passphrase you chose
   ```

   Keep the private key and passphrase only in a password manager and in the two
   secrets; never in the repository or a chat. Rotate before the two-year expiry
   (`gpg --quick-set-expire`, re-export, re-send to the key server).
4. **Environment.** The workflow runs in the GitHub environment `maven-central`;
   create it under *Settings → Environments* and, if you want a second pair of eyes,
   add yourself as a required reviewer so every publish waits for an approval click.

## Each release

1. The Release workflow has published the tag and its assets (four native
   tarballs, the jar, sources and javadoc jars).
2. *Actions → Publish to Maven Central → Run workflow* with the tag and `publish`
   left **off**. The run validates the bundle and stages it.
3. Review the deployment at <https://central.sonatype.com/publishing/deployments>:
   the coordinates, the four native libraries inside the jar, the signatures. Press
   **Publish**, or **Drop** and fix. Publishing is irreversible: a version on Central
   can never be changed or removed, only superseded.
4. The artifact appears at <https://central.sonatype.com/artifact/org.adbcbridge/adbcbridge>
   within a few minutes and on mirrors within hours.

Once you trust the pipeline, run with `publish` **on** to skip step 3.

## What the POM guarantees

Central validates the bundle before it accepts it: `name`, `description`, `url`,
`licenses`, `developers` and `scm` are all present; sources and javadoc jars are
produced on every build; every file carries a `.asc` signature plus MD5 and SHA-1
checksums (the plugin adds those). The version must equal the tag without its `v`,
which the workflow checks first.
