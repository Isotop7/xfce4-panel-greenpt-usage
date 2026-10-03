# GreenPT Credits — xfce4-panel plugin

xfce4-panel button showing remaining GreenPT API credit, read from the `x-credits-remaining` response header.

**Free polling:** chat-completions request with bogus model `__balance_poll__` → `400 Unsupported model`, unbilled, balance header still present.

## Build requirements

GLib >= 2.68, libcurl >= 7.32.0 (enforced via pkg-config). Fedora (one-time, sudo):

    sudo dnf install -y xfce4-panel-devel gtk3-devel libcurl-devel

## Build & install (user-local, no root)

    make
    make install

Installs:

- `~/.local/lib64/xfce4/panel/plugins/libgreenpt.so`
- `~/.local/share/xfce4/panel/plugins/greenpt.desktop`
- `~/.local/share/icons/hicolor/scalable/apps/greenpt.svg`

Libdir suffix derived from panel's pkg-config data as libdir relative to prefix (`lib64` Fedora, `lib/x86_64-linux-gnu` Debian/Ubuntu).

## Use

1. `xfce4-panel -r` (rescan plugin dirs)
2. Panel ▸ Items ▸ + ▸ "GreenPT Credits"
3. Right-click item ▸ Properties: API key (account.greenpt.ai), region (EU/US), refresh interval seconds (default 300, min 10), low-balance threshold (default 5.00).

No key → button shows **"Missing API Key"**. Enter key in Properties, or enable **"Use $GREENPT_API_TOKEN environment variable"** — reads key from panel session env, not stored in rc file (env wins when enabled).

Label: balance, 2 decimals, `€` suffix; red below threshold. Tooltip: region, value, time of last successful update; errors explain what failed (invalid key, network timeout, …) while keeping last value on screen.

## Notes

- Out-of-process external plugin (wrapper-2.0); crash can't take down panel.
- API key stored as plain text in plugin rc file (`~/.config/xfce4/panel/`), like any Xfce plugin config.
- Rebuild/reload: `make && make install && xfce4-panel -r`. Stale wrapper process → remove and re-add panel item.
- Uninstall: `make uninstall`, then remove panel item.

## Config file

`~/.config/xfce4/panel/greenpt-*.rc`, keys under `[greenpt]`: `api_key`, `region` (`eu`|`us`), `refresh_seconds` (legacy `refresh_minutes` migrated on load), `low_threshold`, `use_env_token` (read `GREENPT_API_TOKEN` from env).
