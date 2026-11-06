# Security Policy

[中文](SECURITY.zh.md)

## Scope

This policy covers the code in this repository: the reference decoders and reference encoders, the other command-line tools, the player plug-ins and the Java library. LAE files are untrusted input. A security issue exists if a decoder or plug-in, when given a crafted file, crashes, reads or writes out of bounds, hangs or allocates memory without limit.

The format specifications are in scope where the defect lies in the specification itself, for example, a field whose stated limits allow unbounded memory use.

## Reporting a vulnerability

Report vulnerabilities privately through GitHub: open the repository's **Security** tab and choose **Report a vulnerability**. Do not open a public issue for a vulnerability.

Include, where possible:

- the affected component and version or commit (see [docs/VERSIONS.md](docs/VERSIONS.md));
- the platform and build;
- a file or the steps that reproduce the problem;
- the observed effect.

## Response

This is a personal project maintained by one author. Reports are acknowledged as soon as practicable. A fix is developed in a private advisory, checked with the reported file or steps and against the conformance vectors, and published together with the advisory. Reporters are credited unless they ask not to be.

## Supported versions

Only the latest release of the repository (one row of the release table in [docs/VERSIONS.md](docs/VERSIONS.md), which names the version of every component) and the current `main` branch receive security fixes.
