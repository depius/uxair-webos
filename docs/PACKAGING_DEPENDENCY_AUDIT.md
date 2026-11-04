# Packaging CLI dependency audit (2026-11-03)

Scope: the unchanged locked host-only `@webos-tools/cli` 3.2.6 graph used to package UxAir 1.2.0. This is separate from the ARM application dependency manifest.

A read-only `npm audit --prefix tools/webos-cli --omit=dev --json` returned exit 1 and 18 affected package entries: 1 critical, 4 high, 13 moderate. These counts include transitive/meta-vulnerability propagation, not 18 demonstrated exploitable paths. The graph includes the CLI publisher's nested shrinkwrap and bundled tooling. Raw output is retained locally in ignored `build/packaging-tools-audit.json` and contains no application state or credentials.

| Finding | Assessment for this build process |
| --- | --- |
| proxy-addr <2.0.8, critical GHSA-jqcg-44mw-7w3h | Upstream describes spoofed client IP when a running HTTP application configures an incorrectly specified trusted IPv6 subnet. CI invokes version/package commands and starts no CLI HTTP server or trust-proxy policy. No exploitability of this path in package-only CI was established. |
| body-parser 2.3.0, high GHSA-27r3-jxmh-p828 | Upstream describes a process crash from compressed HTTP request bodies. The CLI's lib/base/server.js uses Express/body-parser in runServer; ares-package does not invoke that server. Do not treat the full CLI's server functionality as assessed for safe use. |
| brace-expansion (multiple nested versions), high; js-yaml, high | Malformed-input CPU/stack exhaustion advisories. The package pipeline uses committed project/staging inputs, not a service accepting arbitrary users' projects. A deliberately untrusted workspace/build host remains outside the trust boundary; these advisories are not declared fixed. |
| Remaining propagated/moderate findings | Recorded for future targeted CLI/upstream maintenance, including eslint tooling, glob/rimraf/temp and qs. No automatic npm audit fix was run. The audit suggested a CLI downgrade for some meta-vulnerabilities; that is not evidence that the older CLI is an appropriate remediation. |

Primary maintainer advisories: [proxy-addr](https://github.com/jshttp/proxy-addr/security/advisories/GHSA-jqcg-44mw-7w3h), [body-parser](https://github.com/expressjs/body-parser/security/advisories/GHSA-27r3-jxmh-p828). The local registry audit also records the brace-expansion/js-yaml advisory identifiers and affected nodes. Source trace: CLI bin/ares-package.js, lib/package.js and lib/base/common-tools.js versus lib/base/server.js:37-63.

Disposition: retain the stable CLI version and exact integrity-pinned graph for this minimal branch, with lifecycle scripts disabled and no server commands in CI. Locking dependencies improves repeatability; it does not repair their vulnerabilities or attest benignness. The CLI and node_modules are excluded from both the TV IPK and corresponding-source archive; only package/lock manifests are retained in source. These host advisories concern host packaging tools, not the TV runtime.

A future change may update or reduce host packaging dependencies after targeted compatibility tests on both archive construction paths; that work must not be mixed with a speculative media/backend refactor. Review the remaining risks as part of normal release approval.
