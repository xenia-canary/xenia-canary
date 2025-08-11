<p align="center">
    <a href="https://github.com/xenia-canary/xenia-canary/tree/canary_experimental/assets/icon">
        <img height="256px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" />
    </a>
</p>

<h1 align="center">Xenia Canary - Xbox 360 Emulator</h1>

Xenia Canary is an experimental fork of the Xenia emulator. For more information, see the
[Xenia Canary wiki](https://github.com/xenia-canary/xenia-canary/wiki).

Come chat with us about **emulator-related topics** on [Discord](https://discord.gg/Q9mxZf9).
For developer chat join `#dev` but stay on topic. Lurking is not only fine, but encouraged!
Please check the [FAQ](https://github.com/xenia-canary/xenia-canary/wiki/FAQ) page before asking questions.
We've got jobs/lives/etc, so don't expect instant answers.

Discussing illegal activities will get you banned.

>[!WARNING]
>This fork serves as a proof of concept for Apple Silicon Macs.<br>
>With macOS 15, Apple has introduced AVX support for Rosetta 2, but their feature set exposure is questionable at best.<br>
>This fork removes the AVX check from Xenia Canary, allowing it to run on said Macs.<br>
>Which obiously wouldn't have to be done if Apple implemented things right.

## Status

Buildbot | Status | Releases
-------- | ------ | --------
macOS (patched Windows executable for Wine usage) | [![CI](https://github.com/mavethee/xenia-canary-no-avx-check/actions/workflows/Windows_build.yml/badge.svg?branch=canary_experimental)](https://github.com/xenia-canary/xenia-canary/actions/workflows/Windows_build.yml) | [Latest](https://github.com/mavethee/xenia-canary-no-avx-check/releases/latest) ◦ [All](https://github.com/mavethee/xenia-canary-no-avx-check/releases)
Netplay builds | | [Stable](https://github.com/mavethee/xenia-canary-no-avx-check/releases/tag/v4.0.0) ◦ [Latest](https://github.com/mavethee/xenia-canary-no-avx-check/actions?query=branch%3Anetplay_canary_experimental)

>[!NOTE]
>More information about netplay can be found on [Netplay Wiki](https://github.com/AdrianCassar/xenia-canary/wiki).

## Quickstart

See the [Quickstart](https://github.com/xenia-canary/xenia-canary/wiki/Quickstart) page.

## FAQ

See the [frequently asked questions](https://github.com/xenia-canary/xenia-canary/wiki/FAQ) page.

## Game Compatibility

See the [Game compatibility list](https://github.com/xenia-canary/game-compatibility/issues)
for currently tracked games, and feel free to contribute your own updates,
screenshots, and information there following the [existing conventions](https://github.com/xenia-canary/game-compatibility/blob/master/README.md).

## Building

See [building.md](docs/building.md) for setup and information about the
`xb` script. When writing code, check the [style guide](docs/style_guide.md)
and be sure to run clang-format!

## Contributors Wanted!

Have some spare time, know advanced C++, and want to write an emulator?
Head to [original Xenia Canary repository](https://github.com/xenia-canary/xenia-canary) and contribute! There's a ton of work that needs to be done, 
a lot of which is wide open greenfield fun.

**For general rules and guidelines please see [CONTRIBUTING.md](.github/CONTRIBUTING.md).**

Fixes and optimizations are always welcome (please!), but in addition to
that there are some major work areas still untouched:

* Help work through [missing functionality/bugs in games](https://github.com/xenia-canary/xenia-canary/labels/compat)
* Reduce the size of Xenia's [huge log files](https://github.com/xenia-canary/xenia-canary/issues/1526)
* Skilled with Linux? A strong contributor is needed to [help with porting](https://github.com/xenia-canary/xenia-canary/labels/platform-linux)

See more projects [good for contributors](https://github.com/xenia-canary/xenia-canary/labels/good%20first%20issue). It's a good idea to ask on Discord and check the issues page before beginning work on
something.

## Disclaimer

The goal of this project is to experiment, research, and educate on the topic
of emulation of modern devices and operating systems. **It is not for enabling
illegal activity**. All information is obtained via reverse engineering of
legally purchased devices and games and information made public on the internet
(you'd be surprised what's indexed on Google...).