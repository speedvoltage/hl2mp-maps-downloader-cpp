# HL2DM Maps Downloader

HL2DM Maps Downloader is a fast, multi-source map downloader for Half-Life 2: Deathmatch. It indexes public map repositories, compares them against your local installation, and downloads only the missing maps.

Built in modern C++ with a responsive terminal interface, it supports parallel downloads, automatic decompression, and intelligent source selection based on latency.

## Features

- Multi-source indexing and downloading
- Parallel downloads with configurable threads
- Automatic `.bz2` decompression
- Skip maps already installed locally
- Include and exclude filters
- Persistent source and settings management
- Clean terminal UI powered by FTXUI
- Cross-platform support (Windows, Linux)

## Requirements

- Half-Life 2: Deathmatch installed
- Internet connection
- Supported operating system:
  - Windows
  - Linux

## Usage

Add one or more map sources, configure your HL2DM path, and start indexing or downloading. The program will handle the rest automatically.

## License

MIT License
