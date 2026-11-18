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
  file opens as a document of its own, as before.
- **Another sample rate or channel count**: the session takes the **higher**
  of the two, after a window says what will happen (Cancel leaves everything
  as it was):
  - a file at a higher rate takes the whole session to it - every file,
    the main one and hidden trims included, is resampled and every place on
    the timeline scaled; a file at a lower rate is resampled up to the
    session's. One undo step either way;
  - a file with fewer channels than its track gets silent ones; a file with
    more widens the track, whose other files get silent channels.
  Converted files are written at the next save (at the session's rate and
  channel count), like edited ones.
- **Dropped on another file**, a file goes **in front of it** when the drop
  is in that file's first half - that file and the later ones of the track
  move right to make room - or **right after it** in its second half. The
  same rule applies when a file is moved (the move preview shows where
  everything lands).
- **The same file can be added several times.** Its copies stay identical:
  editing one copy updates the others (each keeps its own window of the
  file, see trimming), as a second history step chained to the edit - one
  undo takes both back. Saving writes the file once.
- **Right-click on the tracks**:
  - **Add Track** adds an empty track at the bottom, with the channel count
    of the track under the mouse. The first file dropped on an empty track
    gives it its own channel count.
  - **Remove File** (on a file other than the main one) takes it out of the
    session. The file on disk is not touched. Its track stays.
  - **Remove Track** (on an empty track) takes the track away.
- **Selection > Move** (**V**; called *Move Files* before issue #41, where it
  also became the tool that selects and moves the drawings; a selection tool
  S, M, L or P puts it down) arms the move tool. Drag a file in the waveform or the spectrogram:
  - left and right changes its time;
  - up and down picks another track with the same channel count, or an
    empty track;
  - the file's start or end **snaps**, within 8 pixels, to 0, to the start or
    end of any other file, or to the playhead (a yellow line shows it).
    **Hold Shift** to move it freely;
  - the outline turns red where the file cannot go: in a track with another
    channel count;
  - **the main file moves too**, along its own track. What touches it -
    markers, saved and active spectral selections, drawings, tables,
    transcription segments - moves with it; what does not stays on the
    timeline. Saved, its position comes back when the file is opened again;
    its own file on disk is unchanged (its RIFF markers stay in its own time).
  - **dragging an edge** of a file other than the main one **trims** it (the
    cursor shows it): the start or the end is hidden, not deleted - drag the
    edge back out to see it again. It snaps like a move, stops at the file's
    real start and end and at its neighbours. Saving an edited, trimmed file
    writes the whole file back, hidden parts included; a trim alone does not
    rewrite the file.
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
- The **footer** shows each channel name once (issue #48): two stereo tracks
  give **L** and **R**, a stereo and a mono track **L**, **R** and **1**. A
  click on **L** shows or hides the left channel of every track together;
  Ctrl+click shows only those channels (again: all of them).
- **Effects > Resample** converts **each track** (issue #48): its channel
  count and mix matrix are those of one track, applied to every track. Two
  stereo tracks resampled to mono become two mono tracks, and stay mono when
  a track is added later. A new sample rate moves every file and trim to the
  new rate. Tracks of different channel counts can only change the rate
  (the channel field is locked).

While a session is open, a bar at the far right of the editor scrolls through
the rows of every file. Drag its thumb to move through them, or drag an edge
to show fewer rows (they get taller) or more. The mouse wheel over the bar
does the same as the edge (issue #48): up for one row fewer, down for one
more.

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

When you **save** the main file, the first time an edited (or converted)
referenced file has to be written a window asks how: **Save as Copies**
writes `<name>_copy.wav` (then `_copy2`, ...) next to the original, which
stays as it is, and the session refers to the copy from then on (later saves
write the copy in place); **Overwrite Originals** writes the files where they
are; **Not Now** saves only the main file and asks again next time. The choice
is remembered with the session, in the main file's metadata
(`"referencedSave": "copies"` or `"overwrite"`). Without a window (AI control,
command line) the files are overwritten.

Then:

- **The main file:** the engine writes only its own region: its channels, from
  0 to its own length. This holds whatever asks for the save: Save, Save As,
  batch actions, the command line or AI control. The composite never ends up
  in the main file.
- **The other files:** every file whose region changed since it was opened or
  last saved is written back (to its own path or to its copy, see above). It goes through a document of
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
    { "path": "take2.wav", "abs": "/data/take2.wav", "lane": 0, "offset": 480000, "length": 240000,
      "trimStart": 12000 },
    { "path": "room.wav",  "abs": "/data/room.wav",  "lane": 1, "offset": 0,      "length": 360000 }
  ]
}
```

Offsets and lengths are in samples; `ownCopy` marks a copy the session made
(Save as Copies); `trimStart` / `trimEnd` are the samples
hidden at a file's start / end (only when trimmed), and `mainOffset` the main
file's place when it was moved (only when not 0). `lanes` lists every track's channel
count, empty ones included (here a third, empty stereo track). When the main
file is opened again, files that are missing, have more channels than their
track, or overlap another file are left out, and the app lists them; a file
at another sample rate or with fewer channels (converted, not saved since) is
converted again and listed too. If none can be read, the entry is kept as it
was.

## Limits of this first version

- Files of one lane cannot overlap: the composite could not tell them apart
  when saving. Put overlapping files in different lanes.
- Audio outside every file (in a gap) belongs to no file and is not saved.
- If an edit adds or removes channels (paste to new channels, split spectral
  selection, ...), the other files are no longer saved: the app says so when
  you save. Undo that edit, or remove those files from the session.
- Adding, moving or removing a file rebuilds the whole composite, and its undo
  step holds the audio before and after. Memory grows with the session.
- Copies of one file: when an edit changes several copies differently (a
  selection across two of them), the first copy changed on the timeline
  wins and the others take its content.
- Per-file gain or mute and crossfades are not planned.

Code: `include/Session.h` (model: lanes, placement, warps, composite, JSON),
`include/SessionOverlay.h` (outlines and the Move drag in both views),
`Application::addSessionFile` / `moveSessionFile` / `removeSessionFile` /
`openSessionOf` / `saveSessionFiles`, `AudioEngine::setSaveRegion`. Tests:
`tests/regression/sessions.inc`.
