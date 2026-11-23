<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->

# DISPATCH Security Policy

## Reporting security issues

Open a [GitHub issue](https://github.com/Spectre-Works/dispatch/issues/new)
with the affected version, component, and a brief description of the impact.
Keep issue reports suitable for public disclosure. Reports must omit
credentials, identifying logs or screenshots, and details that would let
someone exploit the vulnerability.

## Supported deployment

DISPATCH/01 is designed for operation on a trusted local network. The
controller requires its owner key for administrative access. Direct
internet exposure, including router port forwarding, is outside the
supported deployment.

## Repository submissions

Fixtures and examples must use synthetic data. Credentials, production
signing keys, identifiable diagnostic captures, and executed agreements
must stay outside version control.
