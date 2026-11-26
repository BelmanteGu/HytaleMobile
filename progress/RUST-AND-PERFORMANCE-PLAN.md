# Rust server and performance plan

Notes from the 26/11 planning session. Work happens on the `rust` branch; each item has a GitHub issue (labels `rust`, `performance`, `research`).

## Languages (what the game files show)
- **Server:** Java (`HytaleServer.jar`, run on OpenJDK 25 on the phone).
- **Client:** C# / .NET, shipped as a NativeAOT macOS arm64 binary without source. Loaded by our Mach-O loader.
- Both talk over QUIC.

## Decision: server in Rust (branch `rust`)
Why: the JVM server uses about 1 GB and has GC pauses and warm-up. A Rust server could use roughly 100 to 300 MB, less CPU and battery, and start faster. That is an estimate to confirm by measuring.

Stages, each testable with the official client:
1. Document the protocol the client expects (QUIC, handshake, login).
2. Minimal server: the client joins an empty flat world.
3. World: chunks, blocks, saving, terrain generation.
4. Game: entities, NPCs, inventory, combat.
5. Android packaging: arm64 build inside the APK in place of the JDK.

Risks: the protocol changes with game updates; large scope (Minecraft ports to Rust took years and cover a subset). Start from the existing `hytale-server-rust` project (early, no single-player mode, about 1.1 GB at boot when last checked) instead of from zero.

Legal: written authorization from the game's creator must be on file before rewriting from the server's code. It has not been verified here.

## Decision: no Rust client
The client is closed source. Rewriting it means redoing the renderer, UI, animation, audio and every asset format, and requires reverse engineering that the EULA (section 4.1(b)) forbids. The GPU, not the language, is the phone's bottleneck, so the gain is small. Revisit only if the official client stops working.

## C# against Rust
CPU time is usually within 1 to 2 times; memory is where Rust wins (no GC, no object headers). In the client most time goes to the GPU through OpenGL, so the language is not the bottleneck there. Not measured: confirm with the CPU/GPU split issue.

## Client performance on Android (below the closed client)
Today: client -> OpenGL -> MobileGlues (GL to GLES) -> Adreno driver. Only this layer and the loader are ours.
- Vulkan (Zink or ANGLE-Vulkan): an extra layer on a path that is already short; expect equal or slower unless the GLES driver is the bottleneck. Experiment only.
- Likely wins: profile the GPU, shader cache on disk, compressed textures (ASTC/ETC2), remove redundant work in the translation layer, quality option for expensive passes, frame pacing and CPU affinity.

## Server optimization mod (Java plugin)
Hytale supports server plugins. A plugin that limits view distance, throttles far entities and batches saves is a quick win and keeps compatibility; JVM flags can be tuned without a plugin. API not yet researched.

## First step for everything
Measure on the S23: server CPU, memory and GC; client CPU against GPU per frame. Decide from the numbers.

## Findings 26/11: the existing Rust server (DorianCen/hytale-server-rust, MIT)
It is far ahead of the early notes above: about 339k lines, a real 0.7.7 client joins, moves, breaks and places blocks; protocol build 258; 1,577 fast tests. So the plan is to adopt it, not rewrite. Measured on the Mac with the installed game's Assets.zip (3.9 GB):
- `--validate-assets`: 0 failures, 3m14s, peak 428 MB.
- `serve` (insecure mode): listening after about 4 min (asset decode, NPC catalogs 12 s); idle footprint 719 MB (macOS `footprint`).
- Cross-compile works: `cargo ndk -t arm64-v8a --platform 28 build --release --locked -p hytale-server` gives a 25 MB binary in 12 min; it runs on the S23 (`--version`, `--list-modules`).
- Not measured: the Java server on the same machine and world (no fair comparison yet), the Rust server on the phone, a real client joining.
Open problems: boot time and memory come from decoding all 54 asset stores up front (load lazily or cache a decoded form); the server wants an extracted asset tree (3.8 GB) besides Assets.zip, which the phone cannot hold (about 4.8 GB free): find the subset it reads (prefabs, environments, gradients); release clients need `--auth-mode authenticated` (session tokens through HYTALE_SERVER_SESSION_TOKEN / HYTALE_SERVER_IDENTITY_TOKEN).

## Findings 26/11 (later): running the Rust server from the game on the S23
Measured on the S23 (0.7.7 release, official profile):
- Java server in a world: RSS 684 MB steady, peak (VmHWM) 1.4 GB, 178% CPU while booting and recording the AOT cache; boot about 10-20 s.
- Rust server (`hytale-server-rust`, arm64 build, assets extracted to files/rust/assets: Server/ and Common/TintGradients, 2.2 GB): RSS 656-694 MB idle, peak 712-716 MB, 0% CPU idle; boot about 41-50 s (asset decode up front).
- So the gain so far is peak memory and boot CPU; steady memory is about the same; boot is slower.
Integration state (opt-in with `files/rust/enabled`, off by default):
- `server/java` translates the client's command line (`--bind localhost:PORT` -> 127.0.0.1, `--assets=`), starts the Rust server with the session tokens it inherits (`using Hytale session tokens from the environment`, live auth OK) and relays its output; SIGTERM is passed on.
- `ServerSetup` skips adapting the Java jar when Rust is enabled (the 0.8.0-pre.7 server jar no longer has the Quic.ensureAvailability call and broke the adapter).
- OPEN: the client stays on "Booting server..." after the Rust server listens. Printing a line identical to Java's "Hytale Server Booted! [Singleplayer, Fresh Universe] took ..." (and the closing ==== line) does not move it, and the string "Booted" is not in the client binary, so the readiness check is something else. The client also writes to the server's stdin (killing the server gives "Broken pipe" in the client). Next experiment: record what the client writes to the Java server's stdin and what it expects back.
- 0.8.0-pre.7 (build 83): the Mach-O loader segfaults inside HytaleClient (+0x3dcf78, null deref) right after start; not related to the server. To do after the comparison.

## Findings 26/11 (evening): the ready signal, first connection, 0.8 protocol
- Ready signal found in HytaleServer.jar: the client connects after the raw stdout line `>> Singleplayer Ready <<`; progress is `-=|<message>|<percent>`. With it, the 0.7.7 client connected to the Rust server on the S23 ("session accepted", authenticated connection flow, `GameInstance.OnSetupComplete`), then timed out after about 47 s: the single worldgen worker was generating the spawn and the QUIC connection had no keep-alive. Fix: patch `server/rust/0001-quic-keepalive.patch` (keep-alive 5 s, idle 60 s) and `--workers 4`; not retested yet.
- 0.8.0-pre.7 changed the protocol: build 297, CRC 725196534 (Rust speaks 258). Regenerating it needs the Java protocol definitions; decompiling the jar here was blocked by the tool's permission check, so it is left for the user's own machine and decision.
- Mac M1 baseline, 0.8.0-pre.7 Java server 44 s into a world: 1.6 GB footprint, 90% CPU.
- Details and how to continue: Sources/Hyprism.MachOLoader/server/rust/README.md.
