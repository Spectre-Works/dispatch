<!-- SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC -->

# DISPATCH Security Policy

## Reporting security issues

Email suspected vulnerabilities privately to [contact@spectreworks.io](mailto:contact@spectreworks.io).
Include the firmware version or commit, affected component, and impact.
Use synthetic examples and redact credentials, identifying logs, and screenshots.
Use [GitHub issues](https://github.com/Spectre-Works/dispatch/issues/new) for ordinary bugs and feature requests.

This repository contains the DISPATCH/01 developer preview. Include the
exact build when reporting a problem so SPECTRE WORKS can identify the
affected source and firmware.

## Supported deployment

DISPATCH/01 is designed for operation on a trusted local network. The
controller requires its owner key for administrative access. The dashboard
uses HTTP. Use it on a trusted LAN where only trusted devices can reach it.
Keep router port forwarding and public access disabled. The owner key has
no expiry and can be replaced through physical USB pairing when needed.
Choose **Remember this browser** only on a device you control.

## Repository submissions

Fixtures and examples must use synthetic data. Credentials, production
signing keys, identifiable diagnostic captures, and executed agreements
must stay outside version control.
