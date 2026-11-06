# Lot 3 — modèles IA, devices, débruitage et transcription en jobs

Statut : **fait**. Cœur (catalogue, SHA-256, devices stricts) livré par l'agent parallèle
`ai-control-catalog` (voir `lot-3-core.md`) ; ce fichier couvre la fusion et la couche de commandes.

## Fusion

- `ai-control-catalog` fusionnée à `23c5d28` (CI vert sur les trois plateformes de son côté).
  Conflit trivial dans `tests/UnitTests.cpp` (lignes `#include`/`SC_RUN_TEST` voisines), résolu en
  gardant les deux côtés.
- Corrigé en fusionnant : la cible console `SpectroCraftTests` (`tests/CMakeLists.txt`) liste désormais
  `src/AI/Sha256.cpp` hors du bloc IA et `src/AI/ModelCatalog.cpp` dans le bloc IA (signalé par la note
  du cœur, hors de son périmètre).

## Commandes (`src/control/commands/Ai.cpp`, drapeau `AiBuildOnly`)

| Commande | Comportement |
|---|---|
| `ai.models.list` (`Query`) | `ModelCatalog::listJson()` : 32 entrées, moteur, famille, installé, taille, checksum, backends, `installable` + raison |
| `ai.models.install {modelId}` | `installPlan` (refus immédiat : inconnu, non installable, plateforme, busy) puis **job** non mutant ; réponse `{job, sizeBytes}` ; progression + annulation transmises au catalogue |
| `ai.models.verify {modelId}` | job qui re-hache (SHA-256) |
| `ai.models.remove {modelId, confirm}` | `confirm_required` avec l'aperçu (« Removes 148 MB under … ») ; `busy` si un job en cours utilise ce modèle |
| `ai.devices {init}` | sans `init` : rien n'est chargé (`initialised:false`) ; `init:true` → job qui charge OpenVINO et renvoie la liste réelle |
| `ai.setDevice {engine, device, persist}` | job (la liste réelle exige le runtime) ; device absent → `invalid_params {expected:"one of [auto, CPU]"}` ; OpenVINO → `app.aiDevicePref` (+ `saveAiSettings` si `persist`) ; audio.cpp → préférence de session (`ControlState::audiocppBackend`, transmise par les commandes stems/SR du lot 7) |
| `ai.noiseSuppress {modelId, device, attenLimDb, df3PostFilter, copyNoise, autoDownload, select, channels}` (alias `noiseSuppress`) | même moteur que `step.run noiseSuppress` : **job**, une entrée d'historique, résultat `{deviceUsed, description, undoDepth, selection, …}` |
| `ai.transcribe {modelId, language, translate, fast, autoDownload, channels}` (alias `transcribe`) | job, segments **renvoyés** (`segments[]`, `segmentCount`, `storedInDoc:true`) ; le texte ne va jamais dans le journal |
| `ai.batchTranscribe {folder, …}` | job sur `Application::batchTranscribeFolder` (fichiers chargés sans les ouvrir), résumé `{total, done, skipped, failed, failures}` ; dossier soumis au périmètre de chemins |
| `transcription.get` (`Query`) | segments du document, `source ∈ sctt|sidecar|memory|none` |

Règles communes (`src/control/commands/Steps.cpp`, partagées avec `step.run`) :

- **Device strict pour l'API** (demande du coordinateur) : dans le worker, `sc::ai::resolveAiDevice
  ("openvino", pref)` ; un device indisponible fait échouer le job (`invalid_params`), jamais de repli
  silencieux. Sans `device`, la préférence de l'app est figée au lancement. Le résolveur souple
  `sc::resolveAiDevice` reste celui des dialogues et du rejeu d'actions enregistrées (`applyActionStep`,
  correctif du lot 0), par compatibilité.
- Modèle inconnu → `invalid_params {path: "step.params.modelId", expected: "one of [...]"}` ; modèle absent
  avec `autoDownload:false` → `model_not_installed {missing, sizeBytes, hint: "call ai.models.install"}`.
- Les jobs portent l'id de la commande (`ai.noiseSuppress`, pas `step.run`) : `history.list` attribue
  l'entrée à `ai.noiseSuppress` et au client (`source:"api:<client>"`).
- `buildNsFailureDetails` (déplacé hors de `MainWindow.cpp` par le cœur) n'est pas encore joint aux
  erreurs NS de l'API (`details.manualInstall`) : dette mineure.

## Tests (`tests/regression/ai_commands.inc`, « regression: ai_commands »)

Enregistrement des 10 commandes, alias ; liste (≥ 15 modèles, whisper-base avec checksum, assets
audio.cpp non installables hors Windows) ; modèle inconnu ; `remove` sans confirm ; `verify` (job,
`verified:true`) ; `ai.devices` sans/avec init ; `ai.setDevice GPU` sans GPU → `invalid_params` avec la
liste réelle ; `ai.setDevice CPU` appliqué ; NS modèle inconnu, `model_not_installed`, **device GPU absent
→ erreur stricte sans rien appliquer**, NS `auto` → job fait, device rapporté, 1 undo, historique attribué
à `ai.noiseSuppress`/client ; transcription → segments, `transcription.get` cohérent, texte absent du
journal. Skips explicites si un modèle manque (aucun réseau requis).

Résultats : voir la fin du fichier.

## Constats à remonter (SUMMARY)

- **Bug préexistant** (relevé par l'agent du catalogue) : les binaires audio.cpp CUDA 12.4/13.3 ne peuvent
  pas démarrer une fois installés, `installBinaryFromZip` n'extrayant que `audiocpp_cli.exe` et pas
  `ggml.dll`/`ggml-base.dll`. Le catalogue les marque `installable:false`, mais les dialogues Stem/Super
  Resolution les proposent toujours. Non corrigé ici (Windows, à faire avec Max).
- Migration de toute l'IA vers audio.cpp (demande de SuperMax3d) : **non faite**, comme demandé ; l'API
  `ai.*` ne dépend que du catalogue (champ `engine` = donnée), elle survivra à la migration.

## Résultats

macOS arm64, Debug + IA, modèles `denseunet`, `deepfilternet2`, `whisper-base` présents :
« regression: ai_commands » + IA + contrôle : 927/927 ; suite complète `ctest -j 8` : **5717/5717**,
10/10 (dont `regression: ai_catalog` de l'agent du catalogue). Aucun skip dans `ai_commands` sur ce
poste (pas de GPU OpenVINO sur macOS : les cas « GPU absent » ont donc été exercés).
