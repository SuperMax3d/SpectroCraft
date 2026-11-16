# Sessions (experimental)

A session puts other WAV files on the timeline of the file you have open (the
*main file*): one after the other, or in parallel. Audacity and Audition keep
this in a project file; SpectroCraft keeps it in the main file's own metadata
(the `SCMD` chunk, or the `.meta.json` sidecar in external mode), so the main
file *is* the session. Issue #21; the feature is experimental and may change
or go away.

## Using it

- **Add files by dropping them on the tracks** of the open file: they go to
  the track under the mouse, from the time under it. Several files dropped
  together go one after the other. A file dropped on another one takes the
  first free place after it. Dropped anywhere else (or with no file open), a
  file opens as a document of its own, as before. A file must have the
  track's channel count and the main file's sample rate (for now).
- **Right-click on the tracks**:
  - **Add Track** adds an empty track at the bottom, with the channel count
    of the track under the mouse. The first file dropped on an empty track
    gives it its own channel count.
  - **Remove File** (on a file other than the main one) takes it out of the
    session. The file on disk is not touched. Its track stays.
  - **Remove Track** (on an empty track) takes the track away.
- **Selection > Move Files** (**V**; a selection tool S, M, L or P puts it
  down) arms the move tool. Drag a file in the waveform or the spectrogram:
  - left and right changes its time;
  - up and down picks another track with the same channel count, or an
    empty track;
  - the file's start or end **snaps**, within 8 pixels, to 0, to the start or
    end of any other file, or to the playhead (a yellow line shows it).
    **Hold Shift** to move it freely;
  - the outline turns red where the file cannot go: on another file of that
    track, or in a track with another channel count;
  - the main file does not move (yet).
- **Dragging a file's name** (the tag in its top-left corner) moves it
  whatever the selection tool.
- **View > Show File Borders** outlines each file over its rows and writes
  its name in the top-left corner (yellow for the main file). It is on by
  default, and remembered when you turn it off. A plain file (no other file,
  no extra track) never shows a border. With the borders off, two files that
  touch in a track look like one continuous recording.
- Channels are named **by track**: two stereo tracks read L, R, L, R, a mono
  track 1. **Playback** follows the same rule: every left channel plays on the
  left output, every right channel on the right one, a mono track on both
  (each output divides by the number of channels it receives, so the sum
  does not clip).

While a session is open, a bar at the far right of the editor scrolls through
the rows of every file. Drag its thumb to move through them, or drag an edge
to show fewer rows (they get taller) or more.

Every add, move and remove is one undoable step (audio and layout together),
like any edit.

## How it works

While a session is open, the document's audio is the **composite** of every
file:

- Each lane is a group of consecutive channels, the main file's first.
- Each file sits in its lane's channels from its offset on, with silence
  elsewhere.
- Editing, playback, selections, effects, analysis and the spectrogram all
  work on the composite like on any document.
- Two files touching in a lane form one signal, so the spectrogram runs
  across the boundary instead of ending with silence.

When you **save** the main file:

- **The main file:** the engine writes only its own region: its channels, from
  0 to its own length. This holds whatever asks for the save: Save, Save As,
  batch actions, the command line or AI control. The composite never ends up
  in the main file.
- **The other files:** every file whose region changed since it was opened or
  last saved is written back to its own path. It goes through a document of
  its own, so it keeps its markers and other metadata, and like every save in
  SpectroCraft it becomes a 32-bit float WAV. Files you did not edit are not
  touched.

Edits that change the length (delete, paste, time stretch, ...) apply to every
lane at once, as when every track of a multitrack editor is selected. The
files move or change length with them, and undo puts them back. A file whose
whole region is deleted leaves the session; its own file is not touched.

The metadata entry looks like this. Paths are relative to the main file's
folder, with the absolute path as a fallback:

```json
"session": {
  "lanes": [2, 1, 2],
  "files": [
    { "path": "take2.wav", "abs": "/data/take2.wav", "lane": 0, "offset": 480000, "length": 240000 },
    { "path": "room.wav",  "abs": "/data/room.wav",  "lane": 1, "offset": 0,      "length": 360000 }
  ]
}
```

Offsets and lengths are in samples; `lanes` lists every track's channel
count, empty ones included (here a third, empty stereo track). When the main
file is opened again,
files that are missing, have another sample rate, no longer have their lane's
channel count, or overlap another file are left out, and the app lists them.
If none can be read, the entry is kept as it was.

## Limits of this first version

- Every file must have the main file's sample rate. Resample it first.
- Files of one lane cannot overlap: the composite could not tell them apart
  when saving. Put overlapping files in different lanes.
- Audio outside every file (in a gap) belongs to no file and is not saved.
- If an edit adds or removes channels (paste to new channels, split spectral
  selection, ...), the other files are no longer saved: the app says so when
  you save. Undo that edit, or remove those files from the session.
- Adding, moving or removing a file rebuilds the whole composite, and its undo
  step holds the audio before and after. Memory grows with the session.
- Not done yet (PR #24 review, batches 2 and 3): moving the main file,
  trimming a file by dragging its edges, the same file several times,
  inserting a file in front of another on a drop, a different sample rate or
  channel count, and the choice between saving the other files as copies or
  over the originals. Per-file gain or mute and crossfades are not planned.

Code: `include/Session.h` (model: lanes, placement, warps, composite, JSON),
`include/SessionOverlay.h` (outlines and the Move Files drag in both views),
`Application::addSessionFile` / `moveSessionFile` / `removeSessionFile` /
`openSessionOf` / `saveSessionFiles`, `AudioEngine::setSaveRegion`. Tests:
`tests/regression/sessions.inc`.
