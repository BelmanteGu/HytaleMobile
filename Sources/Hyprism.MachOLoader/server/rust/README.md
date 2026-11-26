# Rust world server (branch `rust`)

The singleplayer world server can be the Rust rewrite
[DorianCen/hytale-server-rust](https://github.com/DorianCen/hytale-server-rust) (MIT) instead of the JVM.
It is opt-in and off by default. Plan, measurements and open problems:
[progress/RUST-AND-PERFORMANCE-PLAN.md](../../../../progress/RUST-AND-PERFORMANCE-PLAN.md).

## Status (2026-11-26)

- Works with the **0.7.x** client (protocol build 258, CRC -386561135). On the S23 the 0.7.7 client
  booted the Rust server, got the ready signal, connected, authenticated and reached
  `GameInstance.OnSetupComplete`; it then timed out while the server generated the spawn (fixed by
  the keep-alive patch below, not yet retested on the phone).
- Does **not** work with **0.8.0-pre.7**: its protocol is build 297 (CRC 725196534). The protocol has
  to be regenerated (`tools/protocol-dumper` + `tools/protocol-gen` upstream read the Java protocol
  sources).

## Upstream checkout and patch

```sh
git clone https://github.com/DorianCen/hytale-server-rust.git
cd hytale-server-rust
git checkout 8ff823f480924dbb7ba3ab546a58844ade6a6cc0
git apply /path/to/HytaleMobile/Sources/Hyprism.MachOLoader/server/rust/0001-quic-keepalive.patch
```

`0001-quic-keepalive.patch`: QUIC keep-alive every 5 s and a 60 s idle timeout. Without it the
connection is silent while the world thread generates the spawn (one worldgen worker took longer than
30 s on the S23) and the client drops it ("Connection timed out").

## Build

Desktop (test host):

```sh
cargo build --release --locked -p hytale-server
```

Android arm64 (needs the NDK and `cargo-ndk`):

```sh
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/28.2.13676358
cargo ndk -t arm64-v8a --platform 28 build --release --locked -p hytale-server
# target/aarch64-linux-android/release/hytale-server (about 25 MB)
```

## How the client talks to its singleplayer server

Found in `HytaleServer.jar` (0.7.x and 0.8.x):

- The client starts `<java> ... -jar Server/HytaleServer.jar --client-pid N --bind localhost:PORT
  --assets=<Assets.zip> --singleplayer --owner-uuid=... --owner-name=... --auth-mode=authenticated ...`
  with the session and identity tokens in the environment (the Rust server reads them:
  "using Hytale session tokens from the environment").
- It connects only after the server writes the raw line `>> Singleplayer Ready <<` on standard output
  (`HytaleServer.sendSingleplayerSignal` -> `HytaleLoggerBackend.rawLog`). "Hytale Server Booted!" is
  only a log line; the client ignores it.
- Loading-screen progress is `-=|<Message as BSON JSON>|<percent>` (`reportSingleplayerStatus`), with
  keys such as `client.gameLoadingView.status.settingUp` / `starting`; shutdown is `-=|Shutdown|...`.
- The client does not write to the server's standard input during boot (recorded: empty).
- The Rust server wants a numeric `--bind` (`127.0.0.1:PORT`, not `localhost:PORT`).

## Android layout (app data, `files/`)

```
files/java                 the launcher the client runs (from server/java in the APK); it starts
                           the Rust server when files/rust/enabled exists
files/rust/enabled         opt-in switch (delete it to go back to Java)
files/rust/hytale-server   the arm64 binary
files/rust/assets/         Server/ and Common/TintGradients/ extracted from the instance's Assets.zip
                           (about 2.2 GB; must match the game version)
files/rust/worlddata/      upstream testdata/Default (world-gen data, World.json)
```

Extract the assets on the phone (as the app):

```sh
adb shell run-as org.hyprism.game sh -c 'cd files/rust && mkdir -p assets && cd assets && \
  unzip -q -o /data/user/0/org.hyprism.game/files/.local/share/Hyprism/Instances/<id>/Assets.zip \
  "Server/*" "Common/TintGradients/*"'
```

`HytaleActivity` rewrites `files/java` from the APK at every start, so a test wrapper pushed by hand is
lost on the next start; rebuild the APK to change it.

## Desktop test hook

`desktop-java-hook.sh` replaces the `java` of the official launcher's JRE (keep the original as
`java.real` next to it). With `~/HytaleMobile-rust-ref/rust-enabled` present it runs the Rust server
for singleplayer worlds and logs to `~/HytaleMobile-rust-ref/rust-mac.log`; without it, it runs the
real Java. Adjust the paths (`R=`) for the host. It sends the ready signal the same way as the Android
wrapper.

## Measured (S23, 0.7.7)

| | Java (in world) | Rust |
|---|---|---|
| Memory (RSS) | 684 MB | 656-694 MB |
| Peak | 1.4 GB | 716 MB |
| CPU | 178% while booting | 0% idle |
| Boot | about 10-20 s | about 41-50 s (all assets decoded up front) |

Mac M1, 0.8.0-pre.7 Java server 44 s into a world: 1.6 GB footprint, 90% CPU.

## Next

1. Retest 0.7.7 on the phone with the keep-alive build and `--workers 4`; measure join time.
2. Faster boot: load assets lazily or cache the decoded stores (boot is the weak point).
3. 0.8.0-pre.7: regenerate the protocol for build 297.
4. Stop the Rust server when the client process dies (the client may SIGKILL the wrapper).
