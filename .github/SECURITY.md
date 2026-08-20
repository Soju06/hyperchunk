# Security Policy

## Reporting a vulnerability

**Please do not report security vulnerabilities through public GitHub
issues, discussions, or pull requests.**

Report them privately via GitHub's
[Private Vulnerability Reporting](https://github.com/Soju06/hyperchunk/security/advisories/new):
clear title, affected commit SHA, reproduction steps, and impact. The report
is visible only to the maintainer.

## Scope

hyperchunk is a pure compute library plus CLI tools. The most likely
security-relevant surface is parsing of untrusted input: datapack worldgen
JSON (`reference/` schema paths), NBT, and region/`.mca` files. Memory-safety
bugs (OOB read/write, overflow) in those paths are in scope even when they
"only" crash. Findings that require running the bench/capture tooling on
attacker-controlled golden data are also welcome, at lower priority.

Out of scope: vulnerabilities in Mojang's server/client, in your JVM, or in
build tooling (CMake/GCC) itself.

## Supported versions

Pre-1.0: the only supported version is the current `main` branch. There are
no release backports.

## Response expectations

Solo-maintained project — response is **best-effort**. You can expect an
acknowledgement within a few days and an honest assessment of severity and
timeline after triage. If a report goes unanswered for 14 days, feel free to
ping by filing a (non-detailed) public issue saying a private advisory is
waiting.
