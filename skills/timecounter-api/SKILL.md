---
name: timecounter-api
description: Query a user's TimeCounter Windows activity history from Linux over its read-only HTTP API, and compare daily active, inactive and uncounted time.
---

# TimeCounter API

Use the Windows machine address supplied by the user or `TIMECOUNTER_URL` and the API token supplied by the user or `TIMECOUNTER_TOKEN`. The default base URL is `http://HOST:47831`. If either is missing, ask for it; the token is visible in TimeCounter's tray → Настройки. Never print or save the token in reports or source control.

From Linux, use curl with a bounded timeout:

```bash
curl --fail-with-body --silent --show-error --connect-timeout 5 --max-time 20 \
  -H "Authorization: Bearer ${TIMECOUNTER_TOKEN}" \
  "${TIMECOUNTER_URL%/}/api/history?days=7"
```

`days=N` means **N previous calendar dates plus today**, newest first. Omitted or zero means today only; supported range 0–3660. For “last seven days including today”, pass 6. Do not request more history than needed.

The response contains `generatedAt` (UTC timestamp), `timeZone` (Windows machine's local timezone ID) and `days`, an array of:

- `date`: local calendar date, YYYY-MM-DD.
- `activeSeconds`: session connected and unlocked, within idle timeout (default 5 minutes since last input).
- `inactiveSeconds`: application running, session locked/disconnected or idle; sleep/delayed sampling also counts here.
- `uncountedSeconds`: elapsed day time not observed by the application.
- `elapsedSeconds`: time from local midnight to now for today, full duration for prior dates; DST days may be 23 or 25 hours.

All durations are numeric seconds and can have fractions. Format them as HH:MM:SS, with hours allowed over 23. Future hours of today are excluded. Missing old records appear as uncounted time, not zero work. Do not equate this personal measurement with the employer's official counter; its idle policy can differ. API snapshots can lag the displayed clock by about a second.

401 means wrong/missing token. Connection failure means check that TimeCounter runs **inside the monitored Windows session**, port, firewall and network/VPN reachability. HTTP is unencrypted; use a trusted network, VPN or SSH tunnel. Do not change firewall rules or expose the port publicly as part of querying reports. There are no mutation endpoints.
