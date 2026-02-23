# Cyberpunk 2077 Breach Protocol Auth Module

## Overview

A new auth module `auth_x11_grid` for XSecureLock that replaces the standard
password prompt with a Cyberpunk 2077 "Breach Protocol" hacking minigame
visualization. The actual password is typed normally underneath — the grid UI
is a purely visual/decorative layer.

Each keypress advances a hardcoded path through a 5x5 hex code matrix, filling
a buffer and completing target sequences.

## Usage

```bash
XSECURELOCK_AUTH=auth_x11_grid xsecurelock
```

## Design

- **Hardcoded path**: `HACK_SEQUENCE` defines a fixed path through the grid.
  Every keypress advances along it regardless of which key was pressed.
- **Axis alternation**: Horizontal on even steps, vertical on odd steps
  (per the Cyberpunk 2077 breach protocol mechanic).
- **Security preserved**: The `priv` struct is `MLOCK_PAGE`'d and `memset`'d
  on cleanup, same as `auth_x11`.
- **No double-buffering**: Matches existing `auth_x11` pattern.

## Files

| File | Description |
|------|-------------|
| `helpers/auth_x11_grid.c` | New auth module |
| `Makefile.am` | Build target added |
| `TASK.md` | This file |
