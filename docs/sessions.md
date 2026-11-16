# Sessions (experimental)

A session puts other WAV files on the timeline of the file you have open (the
*main file*): one after the other, or in parallel. Audacity and Audition keep
this in a project file; SpectroCraft keeps it in the main file's own metadata
(the `SCMD` chunk, or the `.meta.json` sidecar in external mode), so the main
file *is* the session. Issue #21; the feature is experimental and may change
or go away.

## Using it

Everything is in **Edit > Session (experimental)**:

- **Add File After...** puts a WAV right after the last file of the main
  file's lane. It must have the main file's channel count.
- **Add File in Parallel...** gives the WAV a lane of its own, starting at 0.
  A lane is a group of channels (rows) below the main file's.
- **Move Files** arms the move tool. Drag a file in the waveform or the
  spectrogram:
  - left and right changes its time;
  - up and down picks another lane with the same channel count.
  - **Shift** snaps the file's start or end, within 8 pixels, to 0, to the
    start or end of any other file, or to the playhead. A yellow line shows
    the snap.
  - The outline turns red where the file cannot go: on another file of that
    lane, or in a lane with another channel count.
  - The main file does not move.
- **Show File Borders** outlines each file over its rows and writes its name
  in the top-left corner (yellow for the main file). With the borders off,
  two files that touch in a lane look like one continuous recording.
- **Remove File** takes a file out of the session. The file on disk is not
  touched.

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
  "lanes": [2, 1],
  "showBorders": true,
  "files": [
    { "path": "take2.wav", "abs": "/data/take2.wav", "lane": 0, "offset": 480000, "length": 240000 },
    { "path": "room.wav",  "abs": "/data/room.wav",  "lane": 1, "offset": 0,      "length": 360000 }
  ]
}
```

Offsets and lengths are in samples. When the main file is opened again,
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
- Not done yet:
  - moving the main file;
  - trimming a file by dragging its edges;
  - per-file gain or mute;
  - crossfades between files.

Code: `include/Session.h` (model: lanes, placement, warps, composite, JSON),
`include/SessionOverlay.h` (outlines and the Move Files drag in both views),
`Application::addSessionFile` / `moveSessionFile` / `removeSessionFile` /
`openSessionOf` / `saveSessionFiles`, `AudioEngine::setSaveRegion`. Tests:
`tests/regression/sessions.inc`.
