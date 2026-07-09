# Phalk Profiles for Nintendo Switch (Client App)

[![Switch Homebrew](https://img.shields.io/badge/Platform-Nintendo%20Switch-red.svg?style=flat-square)](https://github.com/devkitPro/libnx)
[![Language](https://img.shields.io/badge/Language-C%2B%2B-blue.svg?style=flat-square)](https://en.cppreference.com/)
[![JSON](https://img.shields.io/badge/JSON-nlohmann-orange.svg?style=flat-square)](https://github.com/nlohmann/json)

A lightweight Homebrew application (`.nro`) for hacked Nintendo Switch consoles that acts as the local client for the **Phalk Profiles** web ecosystem.

This application extracts playtime data across all local profiles, structures it into JSON format, and uploads it to your [phalk.net/profiles](https://www.phalk.net/profiles/) account.

---

## Prerequisites & Requirements

1. A Nintendo Switch console running custom firmware (Atmosphère).
2. Internet connection active on the console (ensure you are using proper protection such as **90DNS** or **Exosphere/DNS MITM** to avoid Nintendo CDN bans).
3. An active account registered at [phalk.net/profiles](https://www.phalk.net/profiles/).

---

## Installation & Setup Guide

### Step 1: Extract the contents from the zip file in the root of your Switch SD Card
```text
sdmc:/switch/PhalkProfiles/PhalkProfiles.nro
````

### Step 2: On your Nintendo Switch, launch the NRO file
```text
You can launch the application using hbmenu or your favorite hb launcher.
````

## How to Use
1. Turn on your Nintendo Switch and ensure it is connected to the Internet.
2. Launch the Homebrew Menu (hbg) either through the Album app or by holding the [R] shoulder button while launching a retail game (Title Redirection mode).
3. Find and select Phalk Profiles from your app list.
4. The command-line interface will load, showing your current username and masked password status read from the config file.
5. Press the [R] button on your controller to initiate the Export & Upload sequence.
6. Watch the status log on the screen. Once it shifts to ```Data uploaded successfully!```, your public web profile will instantly reflect your local Switch playtimes.
