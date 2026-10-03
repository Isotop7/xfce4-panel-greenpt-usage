# GreenPT Credits — xfce4-panel plugin

Shows the remaining GreenPT API credit in an xfce4-panel button. The
balance is read from the `x-credits-remaining` response header that
GreenPT returns on API calls.

**How it polls without cost:** a chat-completions request with the bogus
model name `__balance_poll__` fails with `400 Unsupported model`, is not
billed, and still carries the balance header.

## Build requirements

Requires GLib >= 2.68 and libcurl >= 7.32.0 (both are enforced via
pkg-config at build time). Fedora packages (one-time, needs sudo):

    sudo dnf install -y xfce4-panel-devel gtk3-devel libcurl-devel

## Build & install (user-local, no root)

    make
    make install

This installs:

- `~/.local/lib64/xfce4/panel/plugins/libgreenpt.so`
- `~/.local/share/xfce4/panel/plugins/greenpt.desktop`
- `~/.local/share/icons/hicolor/scalable/apps/greenpt.svg`

The libdir suffix is derived from the panel's pkg-config data as the
libdir relative to its prefix (`lib64` on Fedora,
`lib/x86_64-linux-gnu` on Debian/Ubuntu).

## Use

1. Restart the panel so it rescans plugin dirs: `xfce4-panel -r`
2. Panel ▸ Items ▸ + ▸ add "GreenPT Credits"
3. Right-click the item ▸ Properties: paste your API key
   (from account.greenpt.ai), pick region (EU/US), set the refresh
   interval in seconds (default 300, min 10) and the low-balance
   threshold (default 5.00).

Until a key is available the button shows **"Missing API Key"**. Either
enter the key in Properties or enable the checkbox **"Use
$GREENPT_API_TOKEN environment variable"** — the plugin then reads the
key from the environment of the panel session and does not store it in
the rc file (the env value takes precedence when enabled).

The label shows the balance with 2 decimals and a `€` suffix, turning
red below the threshold. The tooltip shows region, value and time of the
last successful update; on errors it explains what failed (invalid key,
network timeout, …) while keeping the last known value on screen.

## Notes

- The plugin runs out-of-process (external plugin, wrapper-2.0); a crash
  cannot take down the panel.
- The API key is stored as plain text in the plugin rc file
  (`~/.config/xfce4/panel/`), like any Xfce plugin config.
- Rebuild/reload: `make && make install && xfce4-panel -r`. If the
  wrapper keeps an old process, remove and re-add the panel item.
- Uninstall: `make uninstall`, then remove the panel item.

## Config file

`~/.config/xfce4/panel/greenpt-*.rc`, keys under `[greenpt]`:
`api_key`, `region` (`eu`|`us`), `refresh_seconds` (legacy
`refresh_minutes` is migrated on load), `low_threshold`,
`use_env_token` (read `GREENPT_API_TOKEN` from the environment).
