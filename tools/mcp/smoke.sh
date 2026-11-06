#!/bin/sh
# End-to-end smoke test: real SpectroCraft (open app with --control) <-> sc_client.py.
# Local only (needs a graphical session; on Linux run it under xvfb-run). Not in CI.
#
#   SPECTROCRAFT_EXE=/path/to/SpectroCraft tools/mcp/smoke.sh /abs/path/file.wav
#
# Steps: launch with --control, wait for the discovery file, app.state, doc.open,
# step.run (fadeOut on 0-0.5 s), history.list, history.undo, then quit the app.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python3}
EXE=${SPECTROCRAFT_EXE:?set SPECTROCRAFT_EXE to the SpectroCraft executable}
WAV=${1:?usage: smoke.sh /abs/path/file.wav}
# A function, not a string: the checkout path may contain spaces.
sc() { "$PY" "$HERE/sc_client.py" "$@"; }

"$EXE" --control </dev/null >/tmp/sc-smoke.log 2>&1 &
APP=$!
trap 'kill $APP 2>/dev/null || true' EXIT INT TERM
export SPECTROCRAFT_PID=$APP

i=0
until sc --list >/dev/null 2>&1; do
  i=$((i + 1))
  if [ $i -gt 100 ]; then echo "no discovery file after 20 s (see /tmp/sc-smoke.log)"; exit 1; fi
  sleep 0.2
done
echo "== instance"; sc --list
echo "== app.state"; sc app.state
echo "== doc.open"; sc doc.open "{\"path\":\"$WAV\",\"activate\":true}"
echo "== step.run fadeOut 0-0.5 s"; sc step.run '{"select":{"start":0,"end":0.5},"step":{"type":"fadeOut","params":{}}}'
echo "== history.list"; sc history.list '{"includeJournal":true}'
echo "== history.undo"; sc history.undo
echo "smoke OK"
