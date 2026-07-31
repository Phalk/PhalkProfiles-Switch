# Phalk Profiles for Nintendo Switch

[![Switch Homebrew](https://img.shields.io/badge/Platform-Nintendo%20Switch-red.svg?style=flat-square)](https://github.com/devkitPro/libnx)
[![Language](https://img.shields.io/badge/Language-C%2B%2B-blue.svg?style=flat-square)](https://en.cppreference.com/)
[![JSON](https://img.shields.io/badge/JSON-nlohmann-orange.svg?style=flat-square)](https://github.com/nlohmann/json)

Phalk Profiles is a lightweight Nintendo Switch homebrew client for [phalk.net/profiles](https://www.phalk.net/profiles/).

It exports playtime data for all local profiles, generates a JSON file and uploads it to the user's Phalk Profiles account.

## Requirements

1. A Nintendo Switch running Atmosphère.
2. An active internet connection.
3. An account registered at [phalk.net/profiles](https://www.phalk.net/profiles/).

## Installation

Copy the NRO file to your switch homebrew folder. Example:

```text
sdmc:/switch/PhalkProfiles/PhalkProfiles.NRO
```

The sysmodule is included inside the NRO's RomFS.
Launch `PhalkProfiles.NRO` through hbmenu or another homebrew launcher.

## First-time setup

1. Open `PhalkProfiles`.
2. Select `Set username` and enter your Phalk Profiles username.
3. Select `Set password` and enter your password.
4. Select `Export & upload`.

The status header uses these states:

```text
[ Status: Ready ]
[ Status: Uploading ]
[ Status: Success! ]
[ Status: Upload failed... ]
```

## Automatic uploads
The app can also install a sysmodule for automatic data upload.
After the sysmodule is enabled (you can do it inside the app), Phalk Profiles watches for the current game to close. When the game exits, it exports the latest playtime data and uploads it automatically.

- `Install & enable sysmodule` — installs and enables it when it is missing.
- `Disable & remove sysmodule` — disables and removes it.

After installing, enabling, disabling or removing the sysmodule, restart Atmosphère for the change to take effect.

## Configuration and exported data

Credentials are stored at:

```text
sdmc:/config/PhalkProfiles/config.txt
```

The latest exported JSON is written to:

```text
sdmc:/switch/PhalkProfiles/export.json
```

The background sysmodule log is written to:

```text
sdmc:/switch/PhalkProfiles/sysmodule.log
```

