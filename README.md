# m5paper_claude_display

An ambient e-ink display driven by Claude Code: an M5Paper (4.7" IT8951 e-paper, GT911 touch) that surfaces what Claude is currently doing or has just finished, without the user having to look at the terminal.

## What it is

Claude Code runs long autonomous sessions. The screen is a passive observer surface: tasks, current state, last output, all rendered on e-paper so the device sips power and stays readable on the desk all day. State arrives over BLE from a Mac daemon that listens to Claude Code hooks.

## Hardware

- M5Paper (ESP32 + IT8951 e-paper controller, 540 × 960 monochrome with 16 levels of grey)
- GT911 capacitive touch overlay
- BM8563 RTC for timestamping
- USB-C, battery-backed

## Stack

- ESP-IDF in C for the firmware (e-paper driver, BLE NUS server, touch input, UI)
- Python on the Mac side: `m5paper_ble_daemon.py` translates Claude Code hook events into BLE NUS payloads; `m5paper_hook.py` is the hook entry point
- Custom IT8951 partial-refresh logic to keep e-paper transitions snappy

## Status

Personal hardware project, on my desk. Working end-to-end. The BLE daemon and the firmware speak a simple framed text protocol; the device-side parser is in `main/ble_nus.c`.
