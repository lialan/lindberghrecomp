# Instruments

Every switch here exists because a guess stood in for a measurement.

## Measuring the picture, not the calls

A previous version of this file claimed a game rendered, on the strength of GL
call counts, without reading a pixel. Five diagnoses of the resulting black
frame were wrong before one was right. Every instrument here exists because a
guess stood in for a measurement, and several were wrong before they were
right — the first framebuffer probe read a fixed 64×64 corner of an 800×600
target and called a healthy buffer empty. **Check the instrument against a
known answer before believing it.**

| Variable | Question it answers |
|---|---|
| `LINDBERGH_FBSTATS=1` | how much of the frame is lit, and what each offscreen target holds at its own size |
| `LINDBERGH_TRACE_FRAME=N` | every draw of one frame — bound programs, viewport, scissor, bound textures with their filter state, and the target after each |
| `LINDBERGH_FP_SOLID=1\|2\|3` | replace every fragment program with flat green, a raw texture fetch, or the texture coordinates: does it rasterise, is the texture black, are the coordinates zero |
| `LINDBERGH_DUMP_FP=dir` | the shader text the Cg seam actually supplied, named by program object |
| `LINDBERGH_GLERR=1` | which entry point first raises a GL error |
| `LINDBERGH_CONSOLE=1` | the game's own debug console — what hardware it looked for and why it gave up |
| `LINDBERGH_SHOT=path` | one frame as a BMP |

The most useful of those is the last-but-one. A Lindbergh game narrates its own
startup through `_sDebug::putConsole`, which goes to a console the cabinet has
and a desktop does not. Binding it turns the whole base-board problem from
guesswork into reading.

## Input and sound

| Variable | What it does |
|---|---|
| `LINDBERGH_AUTOPRESS=<hex mask>` | adds cabinet button pulses to `LINDBERGH_AUTOSTART` + `LINDBERGH_AUTOGUN` diagnostic play |
| `LINDBERGH_AUTOPRESS_PERIOD=N` | frames between those pulses (default 120) |
| `LINDBERGH_AUTOPRESS_HOLD=N` | frames held per pulse (default 15, at most half the period) |
| `LINDBERGH_ALSA_METER=1` | logs the decoded PCM peak every two seconds, before mute/volume are applied |

For example, `AUTOPRESS=40` with period 16 and hold 5 repeatedly presses
player one's Action button, useful for pedal minigames. Muted PCM still
consumes queued frames at the device rate; the meter checks decoder output
without making the test audible.
