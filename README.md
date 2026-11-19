# foo_tealscrobbler

> [!CAUTION]
> The code contained in this repository is predominantly LLM-generated, created
> for the express use of the Laurel system, with the probability that there may
> be kerfuffles in both the source files and executable functionality, and is
> thus considered wholly experimental. Here be dragons, if you believe in
> those.

A teal.fm scrobbler for foobar2000 v2, x64. Writes `fm.teal.feed.play` records and `fm.teal.actor.status` now playing records.

## Build

```
msbuild src\foo_tealscrobbler.vcxproj -p:Configuration=Release -p:Platform=x64 -p:PlatformToolset=v145
```

Output: `build\x64\Release\foo_tealscrobbler.dll`.

## Install

Open `foo_tealscrobbler.fb2k-component` with foobar2000 (Preferences > Components > Install, or double-click the file), then restart foobar2000.

## Configure

Preferences > Tools > teal.fm. Enter a handle and an app password.

## Test

```
tools\build_test.cmd
test\test_teal_client.exe
```
