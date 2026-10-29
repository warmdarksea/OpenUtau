# OpenUtau DAW Bridge Plugin

The plugin half of OpenUtau's DAW integration
([`OpenUtau.Core/DawIntegration/API.md`](../OpenUtau.Core/DawIntegration/API.md), protocol
v1.2), built as **VST3**, **CLAP** and **VST2** audio plugins.

Add an instance to a DAW track; it listens on a loopback port and advertises itself through
the discovery directory. In OpenUtau, connect through the DAW integration dialog: the project,
tracks and rendered part audio stream into the plugin, which plays them against the DAW
transport. The transport position, play state and tempo flow back into OpenUtau.

The editor shows the connection status, the synchronized track list, and a playback mode:
**All tracks** reproduces OpenUtau's mix (effective mute, volume, constant-power pan), or a
single track raw pre-fader for one-instance-per-track routing, where the DAW owns the fader.
Synchronized state — including the part audio, up to 256 MiB — persists in the DAW session, so
a project reopened without OpenUtau running still plays its last synchronized vocals.

## Building

```sh
cmake -S OpenUtau.DawPlugin -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Needs the submodules (`git submodule update --init --recursive`) and, on Linux, the usual
JUCE dependencies (ALSA, X11, freetype, libGL). Artifacts land under
`build/OpenUtauBridge_artefacts/Release/`:
`VST3/OpenUtau Bridge.vst3`, `CLAP/OpenUtau Bridge.clap` and `VST/` (VST2, built against the
bundled GPL [FST](3rdparty/fst) headers — no Steinberg SDK is required or fetched).

## bridge-host

`bridge-host` is the standalone conformance harness
(`OpenUtau.Test/Core/DawIntegration/DawRealPluginTest.cs`): the same server and timeline as
the plugin, paced by a fake transport, with a peak meter on stdout.

```sh
./build/bridge-host_artefacts/Release/bridge-host --dir /tmp/bridge-live --rate 48000 --loop
OPENUTAU_BRIDGE_DISCOVERY=/tmp/bridge-live dotnet test --filter DawRealPluginTest
```

`OPENUTAU_BRIDGE_DISCOVERY` overrides the discovery directory for the plugin formats too —
useful in sandboxed hosts whose temp directory is not the user's.

## Licensing

Unlike the surrounding Apache-2.0 application, this subproject is **GPLv3**: it links JUCE
under the GPL option and the GPL FST VST2 headers. The plugin binaries are therefore GPLv3;
the protocol they speak is specified in the Apache-2.0 part of the repository.
