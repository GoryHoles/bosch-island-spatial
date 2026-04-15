# Bosch Island Mumble Spatial Plugin

This plugin keeps a single Mumble plugin and lets the INI choose which spatial server it targets.

## Configuration

The plugin supports both the legacy single-server format and the new profile-based format in `theisle_spatial.ini` and `theisle_spatial_steamdeck.ini`.

Example:

```ini
server_host=141.98.157.235
server_port=8890
active_server=server1
server1_host=141.98.157.235
server1_port=8890
server2_host=83.147.29.99
server2_port=8890
api_key=change_me
enabled=1
debug_log=0
smoothing=0.80
pan_smoothing=0.95
```

## How Selection Works

If `active_server` matches a complete profile such as `server1` or `server2`, the plugin uses that profile's `serverN_host` and `serverN_port`.

If `active_server` is missing, empty, unknown, or points to a profile with incomplete values, the plugin falls back to `server_host` and `server_port`.

The existing keys `api_key`, `enabled`, `debug_log`, `smoothing`, and `pan_smoothing` behave exactly the same as before.

## Switching Servers

To use `server1`, set:

```ini
active_server=server1
```

To use `server2`, set:

```ini
active_server=server2
```

No second plugin is required.
