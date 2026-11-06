# SpectroCraft — consignes pour Claude Code

Éditeur audio/spectral C++20 / JUCE 9. Ce fichier dit comment **piloter l'app** avec les outils MCP
`spectrocraft` et comment **construire / tester** le dépôt. Référence du protocole :
`docs/ai-control/RAPPORT.md` (§6), installation de l'adaptateur : `docs/ai-control/mcp.md`.

## Piloter SpectroCraft (outils MCP `spectrocraft`)

L'adaptateur (`tools/mcp`, déclaré dans `.mcp.json`, copié de `.mcp.json.example`) parle à l'app
**ouverte** par TCP loopback. Rien ne passe par l'interface graphique : l'écran se met à jour en direct.

Démarrage d'une session :

1. `sc_status` : instances détectées, version, documents ouverts. S'il n'y en a pas, le message dit
   pourquoi : app fermée (proposer `sc_launch`) ou app ouverte **sans** contrôle (demander à l'utilisateur
   d'activer *Window > Allow AI control (localhost)*, ou `sc_launch(force=true)` pour une seconde instance).
2. `sc_state` : documents (`docId`), document actif, sélection, vue, transport, jobs en cours.

Vocabulaire :

- **docId** : identifiant entier **stable** d'un document (`sc_state`, `doc.list`). Le paramètre `doc` de
  chaque outil accepte un docId ou un chemin absolu ; absent = document actif. Ne jamais utiliser un index
  de liste. Agir sur un document non actif le rend actif le temps de la commande (`docSwitched:true`).
- **select** : plage d'une étape, `"all"` ou `{start, end}` en **secondes**. Sans `select`, la sélection
  courante de l'app est utilisée. `sc_run_step(type, params, start, end, channels)` le construit.
- **channels** : canaux **0-based** (`[0]` = premier canal). Absent = canaux visibles.
- **Unités** : secondes, Hz, dB. Les niveaux portent toujours leur unité (`dBFS`, ou `dB-SPL` quand le
  document est calibré) : la recopier telle quelle, ne jamais la deviner.
- **Étapes** (`step.run`) : les 20 types d'actions — `silence, fadeIn, fadeOut, normalize, reverse, invert,
  changeGain {db}, timeStretch, catTimeScale, filterEq, envelope, resample, delete, trim, genSilence,
  genNoise, genTone, noiseSuppress, transcribe, python`. Paramètres exacts : ressource
  `spectrocraft://commands` ou `step.validate`.
- **Jobs** : les opérations longues (installation de modèle, débruitage, transcription, gros fichiers)
  renvoient `{"job":"j-N"}` ; les outils attendent la fin en relayant la progression (`wait=false` pour
  rendre la main). `sc_jobs(job)` pour suivre, `sc_cancel(job)` pour annuler. Un seul job modifiant à la
  fois : sinon erreur `busy`.
- **confirm** : les opérations destructives (écraser un fichier, fermer un document non sauvé, chemin hors
  des racines autorisées) échouent d'abord avec `confirm_required`. **Demander à l'utilisateur**, puis
  rappeler le même outil avec `confirm=true`. Ne jamais passer `confirm=true` d'office.
- **Historique** : chaque édition est annulable (`sc_undo`, `sc_redo`, `sc_jump(state)`) ;
  `sc_history` montre les états (opId, params, source `api:…` ou `menu`). La première édition après un
  retour en arrière vide la pile redo (`redoDiscarded` dans la réponse) : le signaler.
- Une réponse peut contenir `eventsSinceLastCall` : éditions faites entre-temps par l'utilisateur (souris,
  `source:"menu"`) ou par une autre session. En tenir compte avant d'enchaîner.

Outils : `sc_status`, `sc_launch`, `sc_state`, `sc_history`, `sc_undo`, `sc_redo`, `sc_jump`,
`sc_run_step`, `sc_run(cmd, params)` (toute commande), `sc_open`, `sc_save`, `sc_close`, `sc_models`,
`sc_install_model`, `sc_devices`, `sc_noise_suppress`, `sc_transcribe`, `sc_measure_levels`,
`sc_third_octave`, `sc_spectrum`, `sc_export_spectrogram`, `sc_jobs`, `sc_cancel`, `sc_headless`, plus un
outil **généré** par commande de l'app (`sc_<domaine>_<commande>`, ex. `sc_selection_set`,
`sc_effect_changeGain`) dont le schéma vient de `app.describe`. Une commande pas encore livrée par la
version de l'app répond `unknown_cmd` : le dire, ne pas contourner.

Erreurs : le texte d'erreur donne le code (`invalid_params`, `no_document`, `busy`, `timeout`,
`needs_ui`, `unsupported_build`, …), l'indice de l'app et la marche à suivre. `timeout` signifie presque
toujours qu'une boîte de dialogue modale est ouverte dans l'app : demander à l'utilisateur de la fermer.

Sans app ouverte : `sc_headless([{cmd, params}, …])` lance `SpectroCraft --json -` (pas de fenêtre,
pas de `session.hello`, jobs attendus en ligne). En terminal, sans le paquet `mcp` :
`python3 tools/mcp/sc_client.py app.state` (diagnostic, voir `--help`).

## Construire et tester le dépôt

```sh
# Debug, Ninja (macOS / Linux) ; Windows : même chose dans un « x64 Native Tools Command Prompt »
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSPECTRO_ENABLE_AI=ON
cmake --build build --target SpectroCraft
ctest --test-dir build -j 8 --output-on-failure          # Linux : sous xvfb-run -a

# un sous-ensemble de la suite embarquée (sous-chaînes du nom de test, séparées par des virgules)
SC_TEST_FILTER=control build/src/SpectroCraft_artefacts/Debug/SpectroCraft --run-unit-tests
#   macOS : build/src/SpectroCraft_artefacts/Debug/SpectroCraft.app/Contents/MacOS/SpectroCraft

# adaptateur MCP (Python 3.11+, sans build C++)
pip install -e "tools/mcp[test]" && pytest tools/mcp/tests
SPECTROCRAFT_EXE=<exe> pytest tools/mcp/tests/test_real_exe.py   # contre le vrai --json
SPECTROCRAFT_EXE=<exe> tools/mcp/smoke.sh /abs/fichier.wav       # bout en bout, app ouverte (local)
```

- `-DSPECTRO_ENABLE_AI=OFF` pour un build léger (pas d'OpenVINO / whisper.cpp).
- Les tests C++ sont dans `tests/UnitTests.cpp` et `tests/regression/*.inc` (contrôle IA :
  `tests/regression/control.inc`), lancés par `--run-unit-tests` ; ctest les répartit en shards.
- Code du contrôle IA : `include/control/`, `src/control/` (registre de commandes, `--json`, serveur) ;
  notes de passation par lot : `docs/ai-control/progress/`.
