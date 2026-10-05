# Astro Bot Rescue Mission on the PC, shown in the headset through Virtual Desktop

## Experimental PC performance build

This fork retains upstream's Quest support and adds PC VR experiments. The fork's initial
settings are **3600x3840 per eye and a 72 FPS cap**. Double-click `Play AstroQuest.bat` to
choose resolution, maximum framerate and field of view before each launch. The choices are
saved in `pc-vr/settings.txt`. Set the actual headset refresh rate separately in Virtual
Desktop; use 72 Hz and turn SSW off for the initial native-refresh test.

SteamVR OpenXR is also supported by the PC host. This fork reserves the guest address
space before loading the OpenXR runtime: the SteamVR startup failure investigated here
occurred because the game's fixed heap address was unavailable after runtime loading.
The title's additional memory for enlarged eye targets is prepared before that reservation.
For SteamVR, start SteamVR and connect the headset, then select SteamVR as the active
OpenXR runtime before launching. The launcher uses the active runtime; it does not switch
runtime or headset refresh settings. Disable SteamVR motion smoothing when measuring
native frame delivery.
SteamVR idle sessions follow its session-state events rather than Virtual Desktop's
no-picture timeout. Startup and an inactive-headset smoke test passed with SteamVR
2.18.2. Headset gameplay and native framerate still need testing. Forcing session
loss reproduced an access violation inside this installation's `vrclient_x64.dll`
during session destruction; disconnect/reconnect recovery is not verified. If that
occurs, restart the application with SteamVR and the headset already connected.

- OpenXR uses a second queue in the same Vulkan graphics family when available, with a
  producer timeline semaphore and copy-completion fence. Set `SHADPS4_XR_SHARED_QUEUE=1`
  for the original shared queue behavior.
- `Run Recovery.bat` sets `SHADPS4_VR_RETRY_SECONDS=15`. This permits another full-refresh
  probe sooner after temporary overload. GPU headroom checks and retry backoff remain, so
  repeated failures can delay another probe up to 120 seconds. Probes may cause judder;
  this does not make a demanding scene cheaper to render.
- `Run Original Recovery.bat` uses the original 600-second recovery gate for comparison.
  The executable also defaults to 600 seconds unless the environment override is set.
- `Run Full Refresh Diagnostic.bat` forces one game frame per headset refresh. This
  diagnostic ignores the selected FPS cap and can reveal throughput hidden by automatic
  half-rate fallback; it cannot guarantee native refresh and may produce uneven motion.
- Windows CPU timing uses `GetThreadTimes`. The launchers enable periodic frame-path
  diagnostics and archive each session's logs and selected settings under `test-logs/`.

The separate-queue build delivered roughly 72 FPS for most of a local 72 Hz session before
falling to 36 FPS. The user reported better recovery with the 30-second test than with
the original ten-minute gate. This follow-up lowers it to 15 seconds; its governor
simulation passed, but the 15-second interval has not yet been validated in a headset.
Release compilation, queue synchronization and
GUI selection/persistence checks passed. Compare identical scenes and settings, including
an easier scene after overload, before drawing performance conclusions.

For a separate GPU-cost experiment, add `msaa=2` to `pc-vr/settings.txt`; the default is 4.
No local game files, personal saves, account configuration or performance logs are published
in this fork. The upstream documentation below describes the original defaults and launcher.

The second way to play (the first, the app that runs on the headset itself, is in
`README-QUEST-VR.md`): the emulator runs on this PC, and the picture goes to the Quest 3 the
way any PC VR game does, through Virtual Desktop. The PC has far more to give than the
headset: the game runs at the console's own 60 frames a second with the console's
multisampling, and draws far larger than the console ever did: 2880x3072 an eye by default,
four times the pixels of its largest size (1440x1536, what a PlayStation 4 Pro uses), chosen
in a small window at every start.

## Playing

1. **The controller goes to the PC**, not to the headset: a USB-C cable, or Bluetooth (hold
   Create and the PS button until the light bar flashes, then add the device in Windows'
   Bluetooth settings). A DualSense remembers one partner: after playing on the headset's own
   app it has to be paired with the PC again, and the other way round. Connected to the PC it
   gives everything the game uses: buttons, sticks, touchpad, motion sensors, rumble, light bar.
   **Paired with the headset instead, it does not work properly**: Virtual Desktop then hands
   the PC a copy of it, a plain DualShock 4 without motion sensors and without touchpad - the
   controller in the game neither turns nor follows you, and the end of a level (swiping Astro
   out of the controller) cannot be done. That is what happened in the first session on
   2026-10-02 (the log says "PS4 Controller"; the console window now warns about it).
   **Without a gamepad on the PC the headset's own controllers play** (see below).
2. **Virtual Desktop Streamer** has to be running on the PC (the launcher starts it if it is
   not), with its own OpenXR runtime, VDXR, as the PC's OpenXR runtime (Streamer window,
   Options). It is, on this PC.
3. In the headset, start **Virtual Desktop** and connect to the PC. In its Streaming settings
   the frame rate to pick is **120**: the game then draws 60 frames a second and each is
   shown for two refreshes, as on a PlayStation VR. (At 90 it draws 45, at 72 36: see "Speed".)
4. On the desktop you now see in the headset, start **`Play Astro Bot VR.bat`** (in this
   folder). It looks for the game in the `games` folder next to it, up to three folders
   down: an unpacked game (a folder with `eboot.bin` in it) or a `.pkg` package, which it
   offers to unpack there first (PkgTool does it, in about a minute; only a package made
   from a dump can be unpacked, not an encrypted one from the PlayStation Store). When it
   finds neither, a window asks where the game is, and what is chosen there is kept as
   `game=` in the settings. A small window comes up next: the **resolution** of each eye (a slider, from the
   console's 1440x1536 up to 3600x3840), the **most frames a second**, and the **field of
   view**; Play starts the game with them (they are kept in `pc-vr\settings.txt`; untick "Show
   this window at every start" to go without it). A console window then says what is found
   and what happens; the game's window opens behind it and shows both eyes' pictures side by
   side, and after a few seconds the headset switches from the desktop to the game.
5. Pick up the controller. Where it is in the game comes from your hands: put the headset's
   own controllers aside (hand tracking on in the headset) and the headset sees your hands
   around the gamepad. Virtual Desktop has to pass hand tracking on to the PC (its setting for
   forwarding tracking data); where it does not, the emulator takes the places Virtual
   Desktop gives the Touch controllers it makes up from the hands while the real ones lie
   unused. With neither, the controller floats at a fixed spot in front of you and only turns
   with the gamepad's motion sensors. The console window says which it is ("The gamepad is
   placed in the game by where the hands are, which ...").

The other order works too: start the game at the PC and put the headset on afterwards. The
game waits up to a minute for the headset (`wait` in the settings) and starts in it; after
that it starts on the monitor and moves to the headset whenever Virtual Desktop connects.

In the game:

- The first screen asks to move the controller into a floating outline. Hold the controller
  up in front of you, where the outline is. (A gamepad without hand tracking is assumed to be
  there and the screen passes on its own.)
- The world map is selected by **looking** at a planet and pressing ✕.
- **Resetting the view**: hold OPTIONS for a second (as on a PlayStation VR), or press the PS
  button. Where your head is then is where you sit as far as the game goes, and the way you
  face is straight ahead. The emulator does it by itself when ✕ is pressed for the first time
  after the headset came up, and when the headset's own "reset view" is used (hold the Meta
  button).
- A controller without a touchpad (an Xbox pad): the right stick stands in for the finger
  (flick it), and the Back / View button for the click. The right stick does that with a
  DualSense as well while nothing touches its pad; the game has no other use for it.
- **Taking the headset off pauses the game**, the way the console does it: the picture goes
  black and the game waits; it goes on where it was when the headset is back on. The same
  happens while Virtual Desktop shows the PC's desktop instead of the game, and when the
  connection to the headset breaks: the game waits, and goes on when it is shown again.
  (If it ever waits although the headset is on and shows the game: `pause=0`.)
- To quit, close the game's window (or the console window).

### With the headset's own controllers

While no gamepad is connected to the PC, the two Touch controllers are the gamepad:

| Touch controller | DualShock 4 |
| --- | --- |
| left stick | left stick (move) |
| A (right hand) | ✕ (jump, confirm) |
| B (right hand) | □ (punch) |
| X (left hand) | ○ (back) |
| Y (left hand) | △ |
| right stick | the finger on the touchpad: push it the way you would swipe |
| right stick pressed in | the touchpad pressed |
| triggers | L2, R2 |
| grips | L1, R1 |
| menu button (left hand) | OPTIONS |
| left stick pressed in | L3 |
| both sticks pressed in | resets the view |

**The right controller is the controller in the game**: where it is and where it points is
where the game's DualShock is, tracked fully (a gamepad only gives its turning, and its
place as far as hand tracking sees the hands around it). At the first screen, hold the right
controller into the outline; gadgets shoot where it points. `controller_hand=left` in the
settings makes it the left one. The game's rumble goes to both controllers. A gamepad that
is connected to the PC takes over at once, and gives the controllers back when it goes.

## Settings

`pc-vr\settings.txt`, one `key=value` a line (the file explains each):

| Setting | Meaning |
| --- | --- |
| `resolution=2880` | the width of each eye's picture: `1440` (the console's largest), `1800`, `2160`, `2520`, `2880` (default), `3240`, `3600`; or `game` for the console's sizes chosen by the game. The game draws a step smaller by itself while the graphics card falls behind |
| `dynamic=0` | hold the game to the chosen size even where the graphics card falls behind |
| `fps=60` | the most frames a second: `120`, `90`, `72`, `60` (default), `45`, `40`, `36`, `30`; what the headset's refresh rate allows (see "Speed") |
| `fov=100` | how much of the headset's field of view the game draws, 70 to 100 percent: 100 fills all of it; less puts the same pixels over fewer degrees, sharper, with a dark border |
| `fov_of=psvr` | `fov` is a percent of a PlayStation VR's field of view (100 by 103 degrees an eye, what the game was made for) instead of the headset's own (`fov_of=headset`, the default) |
| `menu=0` | no window with the main settings at the start |
| `sharpen=0.3` | sharpening of the picture on its way out, 0 to 1. Virtual Desktop has its own on top |
| `msaa=4` | the most samples a pixel gets; default as the console draws it. With `1` the emulator smooths edges itself (unless `antialias=0`) |
| `hands=0` | do not use hand tracking to place the controller |
| `predict_ms=20` | how far beyond the next picture the head position given to the game is predicted (0 to 80 ms) |
| `stick_touchpad=0` | the right stick no longer doubles as a finger on the touchpad |
| `controllers=0` | the headset's own controllers never stand in for a gamepad |
| `controller_hand=left` | which Touch controller is the controller in the game (default `right`) |
| `pause=0` | the game is never made to wait when the headset is off the head or shows something else |
| `surround=0` | fold the game's surround sound down instead of rendering it for the headset's speakers |
| `real_time=0` | let the game count time in frames, as on the console |
| `pace=1` | the older way of saying `fps`: refreshes of the headset a frame is given (takes the place of `fps` when set) |
| `wait=60` | seconds to wait for a headset before starting on the monitor (`0`: start at once, move over when it connects) |
| `headset=0` | do not look for a headset, play on the monitor |
| `game=...` | where the game is, if not in the `games` folder: its `eboot.bin`, its folder, or its `.pkg` (which is unpacked into `games`). The launcher's "Where is the game?" window writes this line itself |
| `env=NAME=value` | extra environment variable for the emulator |

The emulator's own settings (window size, input bindings, log) are in `pc-vr\user\config.json`
and `pc-vr\user\input_config\`. Saves are in `pc-vr\user\home\1000\savedata\CUSA12392` and are
the same format as on the headset: `tools/quest-save-to-pc.sh` copies the save of the
headset's app over (headset on the cable; nothing on the headset is changed, and a save the
PC already has is kept next to the new one).

**Until 2026-10-03 the PC build never read its save**: every start was a new adventure. The
game's thread that looks for system events started before the one that starts the user
service, the emulator handed it the player's login anyway (a console refuses until the service
is started), the game counted the player twice, took them for one who had come back, and kept
the empty save it had in memory instead of reading theirs. Fixed in the emulator; checked with
the headset's save (World 1, levels 1 to 3 done): the title offers CONTINUE and the world map
has the 14 rescued bots.

## What you get

| | PlayStation 4 | Quest 3 on its own (app 0.7) | PC through Virtual Desktop |
| --- | --- | --- | --- |
| Frames a second | 60 | 30 in the levels, 45 in light scenes | 60 (at 120 Hz), up to 120 by the setting |
| Scene size per eye | 960x1080 to 1440x1536 | 816x870 to 1200x1280 | up to 2880x3072 (default), 3600x3840 at most |
| Edges | multisampled | smoothed by a filter | multisampled |
| Head movement | 120 Hz, reprojected | 90 Hz, reprojected | the stream's rate, reprojected in the headset |
| Field of view per eye | PlayStation VR's, 100 by 103 degrees | PlayStation VR's (85% of it from app 0.9 on) | the headset's own, all of it (the simulated Quest 3: 94 by 99 degrees) |

The game is told the field of view of the headset being worn, as the emulator finds it when
the session starts, and draws exactly that: the picture reaches every edge of the view (with a
PlayStation VR's, as on the console, a Quest 3 would show a narrow dark margin far out to the
sides, while degrees beyond its view on the nose side, top and bottom were drawn for nothing).
What the PC cannot change: the picture reaches the headset as a video stream, with the
softness and the delay that brings (Virtual Desktop's own settings decide those: bitrate,
codec, sharpening).

Measured on this PC (Ryzen 7 9800X3D, RTX 5070 Ti) with a simulated Quest 3 at 120 Hz
standing in for the headset (see "Testing without the headset"): title, prologue, world map
and the first level, its heaviest view included (1800 draws and 8 million vertices a frame),
at 60.0 frames a second with every frame shown for exactly two refreshes, the GPU busy 5 to
15% of the time at 1440x1536. Level 1-4 at 2160x2304 an eye: 60 frames a second, the GPU busy
20%; at 2880x3072: 60, busy 29%.

**How the larger sizes come about.** The game has a list of sizes it draws its scene at
(816x870 to 1440x1536 an eye) and makes the pictures it hands to the headset 1440x1536; it
sets aside 200 MB for its render targets and 872 MB of graphics memory for everything. With
`resolution` above 1440 the emulator writes larger sizes into the game as it loads (every
size of the list grown by the same factor, the pictures for the headset too, the pools and
the memory they come from), and gives the emulated console that much more memory: the game
then draws everything at that size itself - a sharper picture, not an enlarged one.

## Speed

The emulated PlayStation VR refreshes when the real headset's picture is due (the runtime
tells: once for every picture it wants), and the game draws one frame for every two
refreshes, as it does on the console. With Virtual Desktop at 120 Hz that is the console's 60
frames a second; at 90 Hz it is 45 and at 72 Hz 36, each frame shown for exactly two
refreshes, and the game is given the time its frames really take, so it runs at its proper
speed whatever the rate (see "Speed" in `README-QUEST-VR.md`). Where the runtime only asks
for 60 pictures a second or fewer (a 60 Hz stream, or Virtual Desktop's Synchronous Spacewarp
making up every other picture itself), the game is given one refresh for a frame instead of
two, which keeps it at 60.

**More than 60 frames a second** (`fps` above 60): the game may draw a frame for every picture of
the headset. A frame takes this PC 9 to 11 ms whatever the scene (it is the emulator's
processor work that takes the time; the graphics card is busy 13% of it), so 72, 80 and
90 Hz are within reach and 120 is not: there the emulator tries once, finds that frames do
not fit, and stays with every other picture (it tries again every ten minutes; a try is a
few seconds of uneven frames). With the simulated headset at 90 Hz the walk through the
first level held 90 frames a second throughout. The same scripted walk, played at 60 and at
90 frames a second (`tools/pc-rate-compare.sh`), does the same things at the same moments:
the game's own time step follows the frame rate. What does not are the few things the game
counts in frames: an animated sign in the level runs half as fast again at 90. That, and
that nothing but the first level was compared, is why it is a setting and not the way
things are: 120 Hz with the game's own 60 frames is what the game was made for.

**Hitches**: in play the longest wait for a frame is 20 to 25 ms (a frame is 16.7); where
scenes change (the title, the map, a level loading) single waits of 50 to 235 ms happen, six
in the five and a half minutes from the start into the first level. The head's movement
does not wait for them: the picture that is there keeps being turned by the compositor.

## How it fits together

```
PC
└─ shadps4.exe (the emulator, one process)
   ├─ the game's x86-64 code, run directly; PSVR, tracker, camera, sound emulated as on the Quest
   ├─ Vulkan on the PC's GPU; every frame the two eyes' pictures are put side by side
   │   ├─ at full size into an image of the OpenXR host            (the headset's picture)
   │   └─ scaled into the window                                   (what the monitor shows)
   └─ OpenXR host (thread "shadPS4:XrHost", src/core/vr/openxr_host.cpp)
       ├─ session with the PC's OpenXR runtime on the emulator's own Vulkan device
       ├─ every picture the runtime asks for: head pose, hands and controllers in, the newest
       │   frame out as a projection layer with the pose it was drawn for and the PSVR's
       │   field of view
       └─ the runtime's compositor turns it to where the head is by the time it is shown
            └─ Virtual Desktop Streamer ── video ──► Quest 3 (Virtual Desktop app)
```

The OpenXR host is to the PC what the Quest app is to the headset, without the process
boundary: no sockets and no shared buffers, the frame is one GPU copy away. The rest of the
emulator does not know which of the two it talks to.

Things that had to be right, for whoever works on this again:

- **The Vulkan device has to suit the runtime.** The runtime names the instance and device
  extensions it needs and the graphics card the headset hangs off; the emulator adds them
  when it makes its device (`vk_platform.cpp`, `vk_instance.cpp`). A headset that connects
  after that still works: the extensions the common Windows runtimes need are enabled
  whenever a runtime is installed.
- **The headset queue is separate when available.** OpenXR uses queue 1 of the renderer's
  graphics family when that family offers more than one queue. Runtime calls and headset
  copies synchronize on their own mutex, so a blocking runtime call cannot hold up the
  emulator's queue 0 submissions. The copy waits on the producer's timeline semaphore;
  its fence must finish before the source image is returned to the renderer. Devices with
  only one graphics queue retain the shared queue and `Scheduler::submit_mutex`.
  `env=SHADPS4_XR_SHARED_QUEUE=1` selects that shared path for comparison. Every ten seconds,
  `Headset queue ...` reports the end-frame mutex wait separately from the runtime call.
- **sRGB.** A runtime takes an 8-bit image that is not of an sRGB format for linear light and
  shows it too bright. The headset's picture is drawn into an sRGB image (the eye pass
  converts back to linear light at its end, `linear_out` in `post_process.frag`) and copied
  to the runtime's sRGB image unchanged.
- **Between two frames of the game** the last picture is submitted again with its pose; the
  runtime keeps using the image released last.
- **The headset coming off** shows as the session leaving the "focused" state. The emulator
  then tells the game what the console tells it (`sceSystemServiceGetStatus`: the system's
  own screen is over the game), which is what makes this game pause. (The headset's own
  "worn" flag, `hmuMount`, does nothing to it.) While it waits, the game hands over a black
  picture of one pixel an eye; the host shows nothing then instead of asking the runtime for
  images of that size.
- **A runtime that asks for no pictures** shows something else in the headset: the game is
  told to wait, as when the headset is off.
- **A headset that goes away** is not announced by Virtual Desktop's runtime (its source: a
  lost headset only makes it stop asking for pictures, for as long as the session lives;
  but it also asks for none while it shows the desktop, which passes by itself, and a new
  session pulls the headset back into VR). So the host makes a new session when the headset
  was put on and no picture has been asked for since, nor is for 3 seconds; when the
  headset is off and nothing was asked for for 30 seconds (either less and less often, up
  to every 5 minutes, while it changes nothing); or when the session's calls only fail for
  2 seconds. A runtime that does announce a loss is believed at once. The game is paused
  from the loss until a new session is asked for pictures with the headset on the head.
- **The headset's controllers** are read through one OpenXR action set bound to the Touch
  profile (which is also what Virtual Desktop's runtime offers for every controller), made
  with each session, and handed on as the first player's gamepad the way a scripted or
  remote gamepad is (`GameController::ApplyRemoteState`); the aim pose of one of them is the
  tracked controller (`Runtime::UpdatePad`).
- **Sound** goes to the device the runtime names for the headset (Virtual Desktop has its
  own), and the game's 7.1 mix is rendered for two speakers at the ears as on the Quest; the
  microphone is the one the runtime names. Both fall back to Windows' default devices.
  The sound follows its device: when that goes away the sound moves to Windows' default
  device, and back when it is there again (see "Fixed in 0.13").

## Fixed in 0.13 (2026-10-03)

- **No sound until the emulator was started again.** Virtual Desktop takes its playback
  device ("Virtual Desktop Audio") out of Windows whenever the headset is not being streamed
  to, and puts it back afterwards. A stream opened on a device that goes away plays into
  nothing from then on, without an error, and the device that comes back is a new one to the
  sound library. The emulator now looks once a second for the device its sound is meant for:
  three seconds after that went away the sound moves to Windows' default device, and three
  seconds after it is back, to it again. Where the device is Windows' default itself, the
  sound is opened on "the default", which the sound library moves by itself. (The waiting is
  there because opening a device while Windows changes its default can hang the sound for
  good.) The log says which devices come and go ("Audio output added", "removed", "went
  away", "is there"). Tested by taking a playback device out of Windows and putting it back
  while the game plays, and measuring what each device plays: `tools/pc-audio-device-test.sh`.
  Not changed: the microphone.
- An emulator crash now leaves its call stack in the log (module and place in it).

## Fixed on 2026-10-03 (afternoon)

- **The game stopped at start with the DualSense paired to the PC** ("Unhandled Exception
  code 0xc0000005 at 0x800c47a21"): two gamepads were there, the DualSense and the Xbox 360
  controller Virtual Desktop makes of the headset's controllers. Each logged in a player of
  its own, the game registered both with the PlayStation VR tracker, and the emulator's
  tracker refused the second; the game takes that as fatal. Now, with a headset, there is one
  player: the best gamepad (a DualSense before anything else) is theirs and the others are
  left alone (the console window says "... is not used"); `env=SHADPS4_VR_ONE_PLAYER=0` in the
  settings logs in a player for every gamepad again. The emulated tracker also takes up to
  four controllers now, as the console's does.
- **Everything in shade too dark** (most visible in level 1-4, where the ground was black):
  objects lit by any light probe but a level's first were lit from the wrong cube faces. See
  `README-QUEST-VR.md`, "App 0.10".
- **Field of view: 100% is now all that the headset being worn shows**, not a PlayStation
  VR's view. The emulator reads the headset's view from the OpenXR runtime when the session
  starts (the console window says "The headset shows ... degrees an eye") and tells the game
  that, scaled by the `fov` percent. The game asks once, as it starts; should the headset not
  have said by then, the emulator waits up to 10 seconds for it, and after that takes what the
  headset showed the last time (kept in `pc-vr\user\vr_headset_fov.json`). `fov_of=psvr` in the settings goes back to the
  PlayStation VR's view.

## When something does not work

The console window shows the emulator's lines about the headset as they come; the whole log
is `pc-vr\user\log\shad_log.txt` (the start before it: `shad_log.prev.txt`). What to look for:

| Line | Meaning |
| --- | --- |
| `Headset found through VirtualDesktopXR ...` | the runtime has a headset: Virtual Desktop is connected |
| `Waiting up to 60 s for a headset ...` | the runtime has none yet: connect Virtual Desktop |
| `... has no headset connected yet` / `No headset yet` | still none: the game is on the monitor and moves over when there is |
| `Headset session: focused` | pictures are shown and the headset is on the head |
| `The headset is off the head, or shows something else: the game waits` | the game is told to pause, until `The headset is on the head and shows the game` |
| `Frames are shown through 3 images of 2880x1536` | the picture's size and format in the runtime |
| `Headset: the title delivered 60.0 frames a second (20 ms the longest wait for one, 0 waits of more than 50 ms), 60.0 were shown, the headset took 120.0 pictures a second` | every ten seconds: the game's frame rate against the stream's, and how badly it hitched |
| `pictures were shown N degrees from where they were drawn for` | how much the compositor had to turn pictures: a few degrees while the head turns, next to nothing at rest |
| `Hands: both seen N% of the time ... holding the controller N%` | what hand tracking gave |
| `Controller 1 connected: ... (motion sensors yes, touchpad yes, light yes)` | what Windows handed over of the gamepad |
| `No gamepad is connected to the PC: the headset's controllers stand in for it` | the Touch controllers are the gamepad |
| `Controllers: standing in for the gamepad; the right one tracked N% of the time ...` | every ten seconds: where the tracked controller is and what is pressed |
| `The headset is gone: the game waits until it is back` / `... starting over with it` | the session was lost; a new one is made when the headset answers again |
| `The headset is driven by another graphics card ...` | the PC has two; set `Vulkan/gpu_id` in `config.json` or start with the headset connected |
| `Windows has ... of memory left to hand out, and the emulator asks for about 14 GB` | (the launcher, before the game starts) the emulator has Windows set the console's whole memory aside in one request (`address_space.cpp`), 14 GB in all at 2880x3072. Windows can usually still find it by enlarging its page file, but may refuse while it does, and the emulator then stops as it starts (`The emulator ended with code -2147483645`). Close other programs, or just start again |

No picture in the headset although the session is focused: look for `failed:` lines (frame
calls the runtime refused are counted and logged).

## Testing without the headset

Virtual Desktop's runtime has no headset to offer unless one is connected, so the OpenXR path
is tested against the **Meta XR Simulator** (installed on this PC), which is a full OpenXR
runtime with a simulated Quest 3 and its controllers. `XR_RUNTIME_JSON` makes one process use
it; the system's runtime stays Virtual Desktop's.

| Tool | What it does |
| --- | --- |
| `tools/pc-xrsim-test.sh <name> <seconds> [NAME=value ...]` | runs the PC build against the simulator, log in `build/dev/xrsim/<name>/`. `XRSIM_HZ=120` sets the simulated display's rate; `SHADPS4_INPUT_SCRIPT=` as for `run-win.sh` |
| `tools/window-shot.ps1 [<title> <png>]` | lists windows, or saves a picture of one (the simulator's shows what its headset shows; the game's shows both eyes) |
| `tools/xrsim-keys.ps1 <key>:<ms> ...` | works the simulator's window: `B` is A and X, `N` is B and Y, `Y G H J` the sticks, `I` the sticks pressed in, `Comma` the menu button (hold keys for a second: short presses are not always seen); `Look:<dx>,<dy>` turns the simulated head, `Click:<x>,<y>` clicks |
| `tools/pc-rate-compare.sh <name> "<NAME=value ...>"` | the same scripted level walk at 60 frames a second and with the given settings, a picture every two seconds from each |
| `tools/xr-probe-win` (`build/xr-probe-win/xr_probe_win.exe`) | what a runtime offers: extensions, system, Vulkan requirements |
| `tools/tests/launcher-test.ps1` | tries the launcher's search for the game (unpacked games and packages, names with brackets, leftovers of an unpacking) on made-up folders |
| `tools/ui-drive.ps1 -Steps "text\|picture.png\|button\|seconds", ...` | works the launcher's own windows and message boxes from outside: waits for one that shows a text, saves a picture of it, presses a button. With a release unzipped somewhere and a package put in its `games` folder, that is the whole first start, from the question about unpacking to Play |

Settings for tests: `SHADPS4_XR_HEAD=0` (the head is a script's to move, the host only
shows), `SHADPS4_XR_FREEZE_AFTER=<s>` (no new pictures after that), `SHADPS4_XR_WAIT=<s>`,
`SHADPS4_XR_HIDE_FOR=<s>` (the headset only turns up that long after the start),
`SHADPS4_XR_LOSE_AFTER=<s>` (the session is taken for lost once),
`SHADPS4_XR_UNWANTED=<from>,<to>` (no pictures are asked for between those seconds of the
session), `SHADPS4_VR_FASTEST_PACE=1` (what `pace=1` sets), `SHADPS4_XR_CONTROLLERS=`
`0`/`1` (off by default under an input script), `SHADPS4_XR_PAD_OFFSET=<x>,<y>,<z>` (where
the gamepad is taken to be from the controller, in metres right, up and towards the player),
`SHADPS4_OPENXR=0`; in input scripts `worn=0` / `worn=1` take the headset off and put it on.

What the simulator runs showed: the session comes up and is focused; pictures arrive at
2880x1536 in an sRGB image and look right in the simulated headset (upright, colours and
brightness as on the monitor, the narrow margin at the outer edge where the PSVR's field of
view ends; since the game draws the headset's own field of view, the simulator's 54/40/50/49
degrees an eye, the layer it is handed has exactly that one); at 72 Hz the game locks to 36.0 frames a second and at 120 Hz to 60.0, following
a change of rate while running; pictures are shown where the head is at rest (0.0 degrees
off), also after the head was turned and after the seat was reset there; the first press of
✕ resets the seat; closing the window ends the session cleanly; the game's window minimised
changes nothing in the headset. A headset that only turns up 50 seconds into the game is
picked up and shown to; a session lost in the middle of the game pauses it, and the new
session shows it again at 60.0 frames a second; a runtime that asks for no pictures for a
while (as Virtual Desktop's does while it shows the desktop) pauses the game for as long,
without a new session. The simulated Touch
controllers start the game (A), move the stick and the touchpad finger, reset the view (both
sticks), and the right one, held into the outline of the first screen, passes it: the game
draws its DualShock where that controller is.

What only a session with the headset on can show: Virtual Desktop's own runtime accepting all
of it (its source says it will: it passes pose and field of view of the layer on, offers the
image format that is asked for, takes the extensions it is given and maps every controller
to the Touch profile), the stream itself, hand tracking through Virtual Desktop, which sound
devices it names, and what it does when the headset is taken off or disconnected (the host
is written to its source, not to its behaviour).

## Building

```sh
source tools/env-win.sh && cmake --build build/win-x64 --target shadps4   # the emulator
bash tools/make-pc-vr.sh                                                   # copy it to pc-vr/
```

The OpenXR loader (Khronos OpenXR-SDK 1.1.63, `externals/openxr-sdk`) is built with the
emulator; `ENABLE_OPENXR` (on for Windows) switches the whole of it.
