# AGENTS.md

Single-file C xfce4-panel plugin: `src/greenpt-plugin.c` is the entire source. Shows remaining GreenPT API credit in a panel button.

## Build & install

    make            # builds libgreenpt.so via gcc + pkg-config
    make install    # user-local install to ~/.local, no root
    xfce4-panel -r  # restart panel so it rescans plugin dirs

Dev cycle: `make && make install && xfce4-panel -r`. Stale wrapper process → remove and re-add panel item.

- Build deps (Fedora): `sudo dnf install -y xfce4-panel-devel gtk3-devel libcurl-devel` (GLib >= 2.68, libcurl >= 7.32.0, enforced via pkg-config).
- Install path: `pkg-config --variable=libdir libxfce4panel-2.0` minus prefix — suffix is `lib64` (Fedora) or `lib/x86_64-linux-gnu` (Debian/Ubuntu). Never hardcode.
- `libgreenpt.so` gitignored; artifacts land in repo root.

## Architecture

- Out-of-process external plugin (wrapper-2.0); crash can't take down panel.
- Balance poll: chat-completions request with bogus model `__balance_poll__` (POLL_MODEL) → 400, unbilled, `x-credits-remaining` header still carries balance.
- Endpoints: `https://api.greenpt.ai` (EU), `https://api.us.greenpt.ai` (US); `region` rc key selects.
- Config: `~/.config/xfce4/panel/greenpt-*.rc` under `[greenpt]`. API key stored as plain text unless `use_env_token` set → reads `$GREENPT_API_TOKEN` (env wins when enabled).

## No tests, no CI

Manual verification only: clean build (`-Wall -Wextra` in CFLAGS), install, panel restart, check button/tooltip.

## Conventions

- Spell out names: use `_callback` suffix for callbacks, not `_cb`.
