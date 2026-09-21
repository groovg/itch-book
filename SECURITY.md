# Security Policy

## Supported versions

The latest release on PyPI and `main` receive fixes.

## Reporting a vulnerability

Report privately through GitHub Security Advisories on this repository
("Security" tab -> "Report a vulnerability"). Please do not open a public issue for
memory-safety problems in the parser.

Parsing untrusted input is the main risk surface here: the parser reads length-prefixed
frames and skips unknown or mismatched ones by length rather than parsing them, but a
report of an out-of-bounds read or write in `include/` is always in scope. Fuzz targets
under `fuzz/` are a good place to attach a reproducer.
