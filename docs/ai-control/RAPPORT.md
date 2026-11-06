# SpectroCraft — Audit phase 9 : contrôle par IA (VS Code + Claude Code)

Date : 2026-11-05
Dépôt audité : `SpectroCraft` (C++20 / JUCE 9, ~46 800 lignes dans `src/`), lecture seule, aucun build.
Destinataire : Nicolas (transmissible à Max, mainteneur).
Convention : toute affirmation sur le code est citée `fichier:ligne` ; les lignes sont celles du dépôt au 2026-11-05.

---

## 1. Résumé exécutif

- **Verdict** : SpectroCraft est déjà pilotable sans interface (268 occurrences de `Application app` dans `tests/UnitTests.cpp`, CLI `--run-action` sur une `Application` headless à `src/Main.cpp:301`), et le dispatcher « JSON en entrée » existe (`sc::applyActionStep`, `src/Actions.cpp:412`). Mais **aucun** des cinq besoins n'est couvert tel quel : aucun flag n'écrit de JSON, le seul IPC vers l'app ouverte est un tube unidirectionnel « ouvre ces fichiers » (`src/SingleInstance.cpp:38-75`), l'historique n'est qu'une pile undo sans horodatage ni paramètres (`include/UndoPatch.h:53-105`), les mesures sont enfermées dans un `juce::Component` (`src/Panels.cpp:1825`) et le choix de device est en grande partie fictif.
- **Architecture retenue** (unanimité des trois juges) : un **registre de commandes unique** (`sc::control::CommandRegistry`) consommé par quatre façades — serveur TCP loopback NDJSON dans l'app ouverte, mode CLI `--json`, rejeu d'actions, menus — plus un **adaptateur MCP Python mince** (hors de l'app) qui génère ses outils depuis `app.describe`.
- **Premier lot** (2,5 j) : registre minimal + `step.run` générique sur les 20 types d'étapes existants + mode `--json` + journal de session + correction de trois défauts de rejeu confirmés (`envelope` irrejouable, device `"auto"` non résolu, `noiseSuppress` → `normalize` en build non-AI). Jusqu'au lot 5, ce journal ne voit que les éditions passant par `recordActionStep` et les commandes API (voir §5.2 Journal).
- **Contrôle de l'app ouverte** depuis VS Code : atteint à la fin du lot 2 (~9 jours).
- **Effort total** : 8 lots, **23 jours nominaux, fourchette 23-30 j** (hypothèses et marge au §8) ; les lots 3-7 sont indépendants ou différables sans défaire les précédents. La couverture de l'exigence (2) est vérifiable MenuID par MenuID en annexe §11.

---

## 2. Ce que le logiciel sait déjà faire pour un pilotage externe

### 2.1 Tableau des points d'entrée

| Point d'entrée | Fichier:ligne | Headless ? | Thread | Observations |
|---|---|---|---|---|
| Parseur CLI `initialiseInternal` | `src/Main.cpp:148` | oui | message (dans `initialise()`) | un seul flag par processus ; flags inconnus ignorés en silence et l'UI démarre (`:816`) ; tokenisation maison qui supprime tout guillemet (`:158-163`, **toutes plateformes**, aucun `#if`) ; sur POSIX, JUCE recompose en amont `commandLine` depuis `argv` : le JSON inline est impraticable partout |
| `--test-ns-failure-alert`, `--generate-test-files` | `src/Main.cpp:646`, `:807` | oui | synchrone | utilitaires de test (alerte d'échec NS, fixtures) ; texte libre ; non détaillés ici |
| `--run-action <nom> <in> <out>` | `src/Main.cpp:246` | oui | message, synchrone | seul JSON-en-entrée existant ; action cherchée **par nom** dans `SpectroCraft.recent` (`:268-273`) ; magasin temporaire (`:295-301`) ; codes 0/1/2 |
| `--batch <nom> <inDir> <outDir>` | `src/Main.cpp:246` → `src/Actions.cpp:649` | oui | synchrone | résumé texte ; échecs par fichier invisibles dans le code de retour |
| `--test-spectral`, `--test-load` | `src/Main.cpp:197`, `:354` | oui | synchrone | texte libre, code 0 même en échec (`:220-222`) |
| `--test-noise-suppression` | `src/Main.cpp:385` | oui | synchrone | device câblé `"CPU"` (`:436-438`) ; code 0 en échec (`:443-445`) |
| `--test-transcribe` | `src/Main.cpp:453` | oui | synchrone | télécharge le modèle (`:501-502`) ; canal 0 seulement (`:518`) ; résultat imprimé, non persisté |
| `--test-batch-transcribe` | `src/Main.cpp:533` | oui | synchrone | `Application app;` **sans magasin isolé** (`:608`) : écrit le vrai `.recent` |
| `--list-ai-devices` | `src/Main.cpp:760` | oui | synchrone | texte libre |
| `--run-unit-tests` | `src/Main.cpp:784` | oui | synchrone | preuve que toute l'app tourne sans fenêtre |
| Arguments nus (fichiers) | `src/Main.cpp:816` | non | message | seule commande atteignant une app déjà ouverte |
| `sc::singleinstance::Coordinator` (serveur du tube) | `src/SingleInstance.cpp:80` | oui | `juce::Thread` « SC-FileForward » → `MessageManager::callAsync` (`:142`) | hébergé par **toute** instance (`src/Main.cpp:840-845`) ; ack 1 octet **avant** exécution (`:136-137`) |
| `sc::singleinstance::forwardFiles` (client) | `src/SingleInstance.cpp:167` | oui | bloquant 2 s | int32 natifs, pas de magic ni version (`:38-41`, `:63-75`) |
| `Application` (constructeur) | `include/Application.h:38`, `src/Application.cpp:84` | oui | message (c'est un `juce::Timer`) | `useIsolatedSettingsStore` (`Application.h:45`) |
| `sc::applyActionStep(app, step, &err)` | `src/Actions.cpp:412` | oui | synchrone, thread appelant | 20 types ; exige un fichier chargé (`:422`) ; cible = `app.selection`, **pas** le JSON |
| `sc::validateStepParams` | `src/Actions.cpp:346` | oui | aucun | un seul appelant dans l'app : `src/Panels.cpp:11759` |
| `Application::recordActionStep` | `src/Application.cpp:353` | oui | message | 27 sites d'appel dans `src/MainWindow.cpp` ; no-op si `actionRec.active` est faux (`:355`) |
| `Application::undoSteps/redoSteps` | `src/Application.cpp:4175` | oui | message | pile **vidée** sur échec d'`applyPatch` (`:4192`, `:4224`) + alerte asynchrone (`:4170`) |
| `UndoManager::undoAt/redoAt` | `include/UndoManager.h:29-33` | oui | message (deque non synchronisée) | « read-only enumeration for the History tab » |
| `HistoryPanel::refresh` | `src/Panels.cpp:909-930` | instanciable sans fenêtre | message | texte « Loaded <nom> » + description de chaque Patch |
| `Application::noiseSuppress` | `src/Application.cpp:3491` | oui | **bloquant, worker** | commit `copyNoise` via `callAsync`+`WaitableEvent` (`:3603-3613`) ; branche par défaut commit **depuis le worker** (`:3560-3566`) |
| `Application::transcribe` | `src/Application.cpp:3901` | oui | bloquant, worker | `d->setTranscription` **depuis le worker** (`:3951`) |
| `Application::batchTranscribeFolder` | `src/Application.cpp:4006` | oui | bloquant, worker | seul résultat structuré du dépôt (`BatchTranscribeSummary`, `Application.h:24-31`) |
| `Application::runPythonScript` | `src/Application.cpp:3629` | oui | bloquant, worker | hop vers le message thread (`:3876-3886`) ; sortie = WAV uniquement (`:3741-3755`) |
| `sc::ai::ModelManager` (registry/find/isInstalled/ensureInstalled) | `src/AI/ModelManager.cpp:241`, `:319` | oui | `ensureInstalled` bloquant, worker | 15 modèles avec URL + SHA-256 + taille (`:71-133`) ; progression/annulation par callback |
| `sc::acpp::run*` (audio.cpp) | `src/AudioCpp.cpp:650` | oui | bloquant, worker | **Windows uniquement** (stubs POSIX `:713-749`, téléchargement `:367-376`) |
| `StemSeparationWindow::runWorker` / `SuperResolutionWindow::runWorker` | `src/StemSeparation.cpp:1020`, `src/SuperResolution.cpp:550` | **non** | fenêtre + thread détaché + dialogue modal | toute l'orchestration est dans les lambdas |
| `FFTAnalysisView::thirdOctBandsRef` (static) | `src/Panels.cpp:648` (déclaration `include/Panels.h:622`) | oui | aucun (coûteux : FFT 65536 × ≤2048 trames) | seule mesure spectrale totalement séparée de l'UI ; correctif dB-SPL (+98,5 dB `:747`, réf. 2e-5 Pa `:752`) |
| `FFTAnalysisView::computeRegionFrom` | `src/Panels.cpp:1659` | non | message | paramétrée sauf le type de fenêtre lu dans `windowCombo` (`:1694`) |
| `SpectralEngine::computeTile / probeMagnitude / renderToPixels` | `src/SpectralEngine.cpp:207`, `:460`, `:491` | oui | pool interne | dB par trame/bin disponibles ; seule sortie existante = PNG |
| `sc::serialiseBundle / parseBundle` | `src/FileMeta.cpp:262`, `:394` | oui | aucun | le `.meta.json` complet |
| `FileDoc::buildMetaJson` | `include/FileDoc.h:173` | oui | message | sérialiseur JSON du document (marqueurs, sélections, dessins, calibration, tables, transcription) |
| `scttToJson` | `src/ScttChunk.cpp:57` | oui | aucun | segments de transcription |
| `MarkerManager::saveCsv/loadCsv` | `src/MarkerManager.cpp:745`, `:757` | oui | aucun | — |
| Patron « worker → commit sur message thread » | `src/Application.cpp:3603-3613` | — | — | `callAsync` + `WaitableEvent`, `wait(-1)` sûr seulement parce que le message thread n'attend jamais le worker (`:3608-3609`) |
| Harnais de tests : `scPumpUntil`, `ScModalWatchdog`, `setHeadless` | `tests/UnitTests.cpp:207-216`, `:27082`, `:27118` | oui | thread de test = message thread (`ScopedJuceInitialiser_GUI`, `:2482`) | gabarit `testSingleInstance` (`:25849-25922`) pour tester un serveur IPC in-process |

### 2.2 Inventaire de couverture

| Sous-système | Pilotable aujourd'hui | Détail |
|---|---|---|
| **Flags CLI** | 11 flags principaux + 5 sous-options, **0 en JSON** | principaux : `--batch`, `--generate-test-files`, `--list-ai-devices`, `--run-action`, `--run-unit-tests`, `--test-batch-transcribe`, `--test-load`, `--test-noise-suppression`, `--test-ns-failure-alert`, `--test-spectral`, `--test-transcribe` ; sous-options : `--subfolders` (de `--batch`, `src/Main.cpp:338`), `--translate`, `--no-auto-download`, `--meta-mode`, `--channel` (de `--test-transcribe`/`--test-batch-transcribe`, `:571-589`). 3 seulement posent un code de retour fiable (`--run-action`, `--batch`, `--run-unit-tests`) ; 6 flags `--test-*` renvoient 0 même en échec |
| **Types d'étapes d'action** | 20 types (`include/Actions.h:77-96`) ; **19 exécutables** headless | `envelope` validé (`src/Actions.cpp:389`) et enregistré (`src/MainWindow.cpp:5481`) mais aucun `case 'e'` dans `applyActionStep` (cases s, f, n, c, r, i, t, d, g, p à `:427-565`) |
| **MenuIDs** | 91 identifiants fonctionnels (93 énumérateurs `include/MainWindow.h:485-547` moins les sentinelles `RecNoDeviceItem` `:513` et `RecDeviceEnd` `:546`), 90 traités par `menuItemSelected` (`src/MainWindow.cpp:2773-3139`), 1 mort (`ViewColourMap`, `:503`) | 18 couverts par une étape d'action (20 %) ; ~50 autres atteignables par une méthode publique d'`Application` sans UI (≈75 % cumulés) ; le reste exige `MainWindow` : zoom vertical, panneaux, Stem/SuperRes, **sauvegarde** (`FileSave/SaveAs` → `runSaveAsync`, `src/MainWindow.cpp:2318`), **export PNG avec overlays** (`preparePngExport`, `:2536`). **Table complète MenuID → commande JSON → lot : annexe §11** (24 au lot 0, 23 au lot 1, 5 au lot 4, 36 au lot 6, 2 au lot 7, 1 mort) |
| **Modèles IA** | 15 dans `ModelManager` (3 NS OpenVINO + 12 Whisper), tous list/install/exécution headless | + 4 binaires audio.cpp, 9 SAM-Audio GGUF, FlashSR, AudioSR : catalogues sans id stable ni SHA-256 (`src/AudioCpp.cpp:101-166`, `validateDownload` par taille `:205-223`), Windows seulement ; htdemucs non téléchargeables (`:53-75`) |
| **Devices / backends** | 3 vocabulaires | OpenVINO `auto/CPU/GPU/NPU` mais seul le plugin CPU est livré (`extern/OpenVINO/README.md:5-6`) ; Whisper `use_gpu=false` (`src/AI/WhisperTranscribe.cpp:91`) ; audio.cpp `cpu/cuda/vulkan` réel, avec simple avertissement si incohérent avec `variant.txt` (`src/StemSeparation.cpp:497-503`) |
| **Mesures calculées** | **0 exportable en données** | spectre, 1/3 d'octave, courbes, harmoniques : TSV presse-papiers limité à la fenêtre visible et 4 canaux (`src/Panels.cpp:4676`, `:4819`, `:4842`) ; aucun RMS/peak réutilisable (RMS local `static` à `src/MainWindow.cpp:4358`, peak inline dans `normalize` `src/Application.cpp:2829`) |
| **Annotations** | ~90 % JSON-ready | marqueurs, sélections nommées, dessins, tables (avec valeurs), calibration, transcription via `serialiseBundle` / `scttToJson` / `serialiseTables` (`src/Tables.cpp:665`) |
| **Historique** | pile undo par document, 64 niveaux (`include/UndoManager.h:10`) | `Patch` = région + copies audio complètes + chaîne libre `description` (`include/UndoPatch.h:74`) ; ni horodatage, ni id, ni type, ni params ; aucune notification (`include/UndoManager.h:7-59`) |
| **Réglages** | 79 clés dans un seul `PropertiesFile` « SpectroCraft.recent » | `millisecondsBeforeSaving = -1`, jamais relu, réécrit en entier par chaque `save()` (`src/Application.cpp:106-123`, `:350`, `:367`, `:380`) |

---

## 3. Constats vérifiés

Chaque ligne a été relue dans le code par un vérificateur adversarial distinct du lecteur initial. « Partiel » = l'affirmation tient mais nécessitait une correction qui change la conception.

| # | Affirmation | Verdict | Preuve | Conséquence pour la conception |
|---|---|---|---|---|
| 1 | Le tube d'instance unique ne transporte que des chemins et répond par un octet 0x01 avant même d'ouvrir les fichiers | **confirmé** | `src/SingleInstance.cpp:38-41`, `:63-75`, `:136-137` | canal nouveau obligatoire ; le tube reste réservé à « ouvrir des fichiers » |
| 2 | Aucun flag CLI n'émet de JSON ; les `--test-*` quittent sans `setApplicationReturnValue` | **confirmé** | `src/Main.cpp:220-222`, `:443-445`, `:483-486`, `:520-524`, `:630-634` | flag neuf `--json` avec contrat de sortie, plutôt qu'adapter les 11 flags existants |
| 3 | Chaque instance héberge le serveur du tube, `multi_instance` vaut 1 par défaut | **confirmé** | serveur dans chaque instance : `src/Main.cpp:840-845`, `src/SingleInstance.cpp:92` (`mustNotExist=false`) ; JUCE n'impose pas l'instance unique : `moreThanOneInstanceAllowed() → true` (`src/Main.cpp:133`) ; défaut 1 du **réglage** : `getIntValue (kSettingKey, 1)` (`src/SingleInstance.cpp:32`), clé `multi_instance` (`include/SingleInstance.h:18`) ; à 0, `handleStartup` (`:191-213`) forwarde et le processus quitte (`src/Main.cpp:825-832`) | identité par instance : port éphémère + fichier de découverte `<pid>.json` ; `--control` doit court-circuiter `handleStartup` (§7.5) |
| 4 | `applyActionStep` est pure, synchrone, sans UI | **partiel** | pas de `case 'e'` (`src/Actions.cpp:427-565`) ; `noiseSuppress` → `normalize` hors AI (`:436-445`) ; littéral `"auto"` (`:441`) ; dispatch sur le 1er caractère du type (« sxxx » → silence) ; peut lancer Python et télécharger des modèles | compléter et durcir le dispatcher, pas seulement l'envelopper ; toujours appeler `validateStepParams` avant |
| 5 | `--run-action` n'accepte qu'un nom d'action du magasin utilisateur | **confirmé** | `src/Main.cpp:268-291` | l'entrée JSON inline/stdin est le premier verrou à lever |
| 6 | 91 MenuIDs ; 18 par étape, 54 par `Application`, 19 UI pur | **partiel** | 90 traités (`src/MainWindow.cpp:2773-3139`) ; `FileSave/SaveAs` et `FileExportSpectral` n'ont **pas** d'équivalent public dans `Application` (`src/MainWindow.cpp:2316-2340`, `:2536`) ; le zoom horizontal est pilotable via `viewStart/viewEnd` + `ensureTimeRangeVisible` (`include/Application.h:120`, `:483`) | deux ou trois extractions de logique hors de `MainWindow` (save, export PNG) |
| 7 | Un seul point d'enregistrement d'historique : `recordReplace` | **confirmé** | `src/AudioEngine.cpp:843-871` (seul appel à `UndoManager::record`) | horodatage/id/opId s'ajoutent à un seul endroit |
| 8 | `Patch` n'a qu'une chaîne libre comme méta-information | **partiel** | `include/UndoPatch.h:53-105` exact ; **mais** `recordActionStep` capture déjà type + params JSON des mêmes éditions (`src/Application.cpp:353-362`, ex. `{"db":6.0}` à `src/MainWindow.cpp:3808`), opt-in seulement | généraliser ce mécanisme (toujours actif, horodaté, lié au Patch) plutôt que repartir de zéro |
| 9 | `UndoManager` n'a aucune notification ; History panel rafraîchi par polling | **partiel** | aucune notification exact ; mais ce n'est **pas** du polling : rafraîchissement événementiel piloté par `MainWindow` (`refreshAfterEdit` `:3006-3007`, `onStateChange` → `syncSharedState` `:388`) ; le timer 30 Hz (`:504`) ne touche pas aux panneaux | un undo lancé hors UI (CLI, MCP) ne produit aucun signal : ajouter `onChanged` dans `record/popUndo/popRedo/clear` |
| 10 | L'historique est vidé par load, resample, échec d'undo, closeDoc ; les marqueurs seuls n'y entrent jamais | **confirmé** | `src/FileDoc.cpp:173` ; `src/AudioEngine.cpp:1817` ; `src/Application.cpp:4192`, `:4224`, `:857`, `:1961` | journal append-only séparé, qui consigne aussi les vidages et leur raison |
| 11 | Stems et super-résolution n'ont aucune fonction headless | **partiel** | orchestration dans `runWorker` (`src/StemSeparation.cpp:1055-1280`, `src/SuperResolution.cpp:579-673`) ; mais les deux extrémités sont réutilisables : `sc::acpp::run*` (`src/AudioCpp.cpp:650-711`) et `createDocFromMerged/applyStemSeparationResult` (`src/Application.cpp:2296`, `:2347`, `:2475`, testées sans UI `tests/UnitTests.cpp:6821-6931`) | extraire seulement le « milieu » du worker ; aucun binaire audio.cpp macOS/Linux n'est référencé (`binaryChoices` `:127-144`, `binaryPath` `:25-28`) |
| 12 | Deux registres de modèles incompatibles | **confirmé** | `src/AI/ModelManager.cpp:71-133` vs `src/AudioCpp.cpp:101-166` | vue unifiée `{id, engine, family, installed, checksum, platforms}` |
| 13 | Débruitage et transcription pilotables sans UI mais sans JSON | **partiel** | `--test-noise-suppression/--test-transcribe` court-circuitent `Application` (`src/Main.cpp:385-532`) ; les étapes d'action passent `nullptr` comme progression (`src/Actions.cpp:438-489`) ; **le JSON existe déjà** : `scttToJson` écrit dans le chunk SCTT ou le sidecar (`src/Application.cpp:4131`, `:4137`) | travail = exposer ce JSON, pas le créer |
| 14 | Le choix de device est fictif | **partiel** | fictif pour OpenVINO (CPU seul livré) et Whisper (`use_gpu=false`) ; **réel** pour audio.cpp (`--backend` transmis au CLI) ; pour les actions, `"auto"` part tel quel dans `ov::Core::compile_model` (`src/AI/OvRuntime.cpp:169`) alors que ni un device nommé « auto » ni le méta-plugin AUTO ne sont livrés → l'étape `noiseSuppress` en `--run-action` devrait échouer (non prouvé par exécution ; aucun test ne rejoue cette étape, `tests/UnitTests.cpp:26681`) | résoudre par `sc::resolveAiDevice` (`src/AI/OvRuntime.cpp:24`, seul appel réel `src/MainWindow.cpp:6276`) ; publier les devices réels par moteur |
| 15 | Les éditions d'`Application` ne notifient pas l'UI et n'enregistrent pas d'action | **partiel** | vrai pour les **éditions audio destructives** (silence, gain, delete, normalize…) : ni `onStateChange` ni `refreshAfterEdit` ; seul `applySuperResolutionResult` notifie (`:2593`). **Faux en général** : `onStateChange` a 14 sites d'appel dans `src/Application.cpp` — `closeDoc` (`:866`), `startRecording`/`stopRecording` (`:1129`, `:1194`), `timerCallback` (`:1256`), `setSelection` (`:1296`), `clearSpectralSelection` (`:1329`), `refreshSpectralSelectionUI` (`:1359`), `setShowAllDrawings` (`:1521`), clone pick (`:1669`, `:1681`, `:1714`), gestes de sélection (`:1784`, `:1799`). `recordActionStep` appelé uniquement depuis `MainWindow.cpp` ; les hooks d'état annexe sont déclenchés (`onTimeRangeDeleted/Inserted/Stretched`, `onPatchSideStateRestored`, `:2717`, `:2047`, `:2952`, `:4289`) ; seul `refreshAfterEdit` (`src/MainWindow.cpp:724-747`) purge le cache de tuiles | hook `refreshAfterEdit` via `UiBridge` après chaque commande mutante **audio** ; `selection.set`, `doc.close`, `record.*` n'en ont pas besoin, `onStateChange` suffit |
| 16 | Le motif « worker → commit sur message thread » est utilisé par `noiseSuppress` et `runPythonScript` | **partiel** | dans `noiseSuppress` il ne couvre que la branche `copyNoise` (`:3603-3613`) ; la branche par défaut commit depuis le worker (`:3560-3566`), comme `transcribe` (`:3951`) ; seul `runPythonScript` l'applique à tout son commit (`:3876-3888`) | aligner ces deux chemins avant d'exposer `ai.*` en jobs |
| 17 | `AlertGate` est un drapeau global activé seulement par les tests | **partiel** | exact (`include/AlertGate.h:20-33`, `tests/UnitTests.cpp:27118`) ; mais `AlertGate` ne couvre que ses deux wrappers : **60 `runModalLoop` bruts** (32 dans `src/MainWindow.cpp`, 28 dans `src/Panels.cpp`) et des `FileChooser` synchrones (`src/MainWindow.cpp:2304`, `:2423`, `:6659`) le contournent ; le harnais de tests doit se doubler d'un `ScModalWatchdog` (`:27082-27105`). En revanche, les deux alertes d'`Application` (`:1187` `sc::alert::showMessageBox`, `:4170` `sc::alert::showMessageBoxAsync`) **passent** par les wrappers `AlertGate` (`include/AlertGate.h:63`, `:83-91`) : avalées en headless, bloquantes seulement en GUI sans capture | une commande ne doit jamais atteindre les chemins bruts ; capture d'alertes **par portée** (`ScopedCapture`), pas par drapeau global — c'est précisément ce que `:1187`/`:4170` justifient |
| 18 | `computeRegionFrom` lit le type de fenêtre dans le ComboBox | **confirmé** | `src/Panels.cpp:1694` | changement d'une ligne de signature pour la rendre pure |
| 19 | `thirdOctBandsRef` est static et pure | **confirmé** | `src/Panels.cpp:648-754` ; tests `tests/UnitTests.cpp:2688-2775` (≈ 91 dB-SPL pour une sinusoïde pleine échelle à 1 Pa) | réutilisable telle quelle ; les tests servent de référence d'égalité |
| 20 | `doRecompute` exige `getWidth()>0`, prend la région dans la sélection UI et throttle à 60 ms | **partiel** | exact, mais le throttle ne s'applique qu'à quatre conditions réunies (cache existant, région > une fenêtre, calcul précédent coûteux, < 60 ms ; `src/Panels.cpp:1915-1917`) ; `computeTimeCurveFrom/computeSpectrumCurveFrom` existent déjà paramétrées mais privées (`include/Panels.h:796`, `:803`) | l'API « analyseRegion » consiste surtout à exposer et découpler, pas à créer |
| 21 | Aucune sortie JSON pour une mesure calculée | **partiel** | exact pour spectre/bandes/courbes/harmoniques ; mais le dépôt produit du JSON pour actions (`src/Actions.cpp:115`), méta Python (`src/PythonScripts.cpp:620`), transcription, tables ; et `getThirdOctMags()` / `getLiveCurves()` (`include/Panels.h:421`, `:442`) exposent déjà les données brutes sans presse-papiers | le schéma de résultat reste à définir ; les données sont accessibles |
| 22 | Le pont Python ne relit que le WAV de sortie ; stdout est jeté en cas de succès | **confirmé** | `src/Application.cpp:3741-3755` | ajouter un `result.json` optionnel pour marqueurs/mesures |
| 23 | Les sous-processus héritent de stdin | **confirmé** | `src/PythonScripts.cpp:752`, `:859-860` ; `src/AudioCpp.cpp:564` | **exclut un transport MCP stdio dans l'app** |
| 24 | `runProcess` n'a aucun timeout ; en rejeu d'action `onProgress` est `nullptr` | **confirmé** | `src/PythonScripts.cpp:800` ; `src/Actions.cpp:579` ; `src/Application.cpp:3640` | timeout imposé côté dispatcher |
| 25 | audio.cpp est Windows-only (exécution et téléchargement) | **confirmé** | `src/AudioCpp.cpp:713-749`, `:367-376` | annoncer `unsupported_platform` ; non testable sur le Mac de Nicolas |
| 26 | Toute la persistance tient dans un seul `PropertiesFile` jamais relu | **partiel** | exact pour les 79 clés de réglages ; scripts Python, modèles, cache OpenVINO, sidecars `.meta.json` sont des fichiers séparés relus depuis le disque | seul `.recent` exige une commande in-process ; le reste peut être écrit par un tiers |
| 27 | `Application` est pilotable sans `MainWindow` | **partiel** | exact ; mais un mode JSON doit armer `setHeadless`, disposer d'un `MessageManager`, poser `SC_NO_AUDIO_DEVICE`, et `--run-action` n'appelle **pas** `validateStepParams` | ces câblages manquants sont à ajouter, pas à « réutiliser » |
| 28 | Le tube FileForward est le seul IPC vers l'app ouverte | **partiel** | seul IPC **fonctionnel** ; sur macOS JUCE route `application:openFiles:` vers `anotherInstanceStarted`, surchargé par un corps vide (`src/Main.cpp:897`) | sans conséquence pour la conception |

**Réfutations relevées par les juges sur les architectures candidates** (toutes vérifiées dans le code, toutes intégrées au §5) :

- `StreamingSocket::close()` débloque `accept()` en se connectant au membre `portNumber`, resté à **0** après `createListener(0, …)` (`extern/JUCE/modules/juce_core/network/juce_Socket.cpp:157-158`, `:613`) : sur Linux le thread serveur ne s'arrête pas proprement → auto-connexion à `getBoundPort()` avant `close()`.
- `userApplicationDataDirectory` = `~/Library` sur macOS (`juce_Files_mac.mm:210`), pas `~/Library/Application Support` ; cohérent avec `scriptFolder` (`src/PythonScripts.cpp:519-523`) et `modelRoot` (`src/AI/ModelManager.cpp:175-177`).
- `replaceSamples` appelle `ensureEditable` lui-même (`src/AudioEngine.cpp:1391`) : une édition « courte » sur un fichier streamé charge tout le fichier sur le thread de messages ; l'UI l'évite par `runDestructiveEdit` sur worker (`src/MainWindow.cpp:1996-2040`).
- `closeDoc` fait `docs.erase` (`src/Application.cpp:857-859`) : adresser les documents par index est faux entre deux commandes.
- `history.jumpTo` ne vide jamais la pile redo (`popUndo` pousse sur `redoStack`, `include/UndoManager.h:35-42`) ; c'est la première édition suivante qui la vide (`record()`, `:13-18`).
- Basculer `setHeadless(false)` après une commande désactiverait le mode headless global posé par le harnais (`include/AlertGate.h:33`).
- En mode CLI, les commits des jobs passent par `callAsync` (`src/Application.cpp:3610`) : sans pompe de messages, un job ne se termine jamais quand le thread de messages lit stdin.
- `FileDoc::toBundle()` n'existe pas ; l'équivalent est `FileDoc::buildMetaJson` (`include/FileDoc.h:173`).

---

## 4. Manques et risques, par sévérité

### Bloquants (aucun des cinq besoins n'est atteignable sans les lever)

| Manque | Preuve | Besoin touché |
|---|---|---|
| Aucune sortie JSON, codes de retour trompeurs | `src/Main.cpp:241`, `:443` | (4) |
| Pas de canal requête/réponse vers l'app ouverte ; tube unidirectionnel, non versionné, nom fixe partagé | `src/SingleInstance.cpp:38`, `:92`, `:134` | (5) |
| Pas d'entrée JSON externe (nom d'action seulement) | `src/Main.cpp:272` | (2), (4) |
| Vocabulaire d'étapes limité aux effets destructifs ; cible implicite (`app.selection`) ; pas d'open/save/select/undo/marker/export | `src/Actions.cpp:425` ; `selectAll` forcé `src/Main.cpp:309` | (2) |
| Aucun sérialiseur « état de l'app → JSON » | `include/Application.h:53` | (4), (5) |
| Les éditions **audio destructives** d'`Application` ne rafraîchissent pas l'écran (`refreshAfterEdit` dans `MainWindow` ; `onStateChange` n'y est pas appelé, contrairement à sélection/enregistrement/closeDoc) | `src/Application.cpp:2724` (fin de `del`), `src/MainWindow.cpp:724-747` | (5) |
| Calcul du spectre enfoui dans un `Component` dépendant de la géométrie et de la sélection UI | `src/Panels.cpp:1825-1834`, `:1694` | (4) |
| Stems/SR sans fonction headless ; audio.cpp Windows-only | `src/StemSeparation.cpp:1020`, `src/AudioCpp.cpp:713`, `:367` | (3) |
| Le magasin de réglages n'est jamais relu : toute écriture externe est perdue | `src/Application.cpp:109` | (2) |
| Le script Python ne peut renvoyer que de l'audio | `src/Application.cpp:3741` | (4) |

### Importants

- Codes de retour absents sur six flags `--test-*` (`src/Main.cpp:443`) ; flags inconnus ignorés et l'UI démarre (`:816`) ; `--test-batch-transcribe` sur le vrai magasin (`:608`) ; pas de choix de device en CLI (`:436`) ; tokenisation maison qui supprime tout guillemet (`:158-163`, **toutes plateformes**) et, sur POSIX, recomposition de `commandLine` par JUCE depuis `argv` : le JSON inline est impraticable partout, d'où `--json <fichier|->`.
- `envelope` irrejouable (`src/Actions.cpp:425`) ; `noiseSuppress` → `normalize` en build non-AI (`:436-445`) ; device `"auto"` non résolu (`:441`) ; validation incohérente entre `validateStepParams` et `applyActionStep` (`:346`, `:567-576`).
- Rejeu dans l'app ouverte terminé/échoué par boîtes modales (`src/MainWindow.cpp:2076`, `:2089`, `:2056`) ; `noiseSuppress` sans prompt bloquant sur le message thread via `runDestructiveEdit` fast path (`:2197`, `:2000-2007`).
- Historique : pas d'horodatage ni d'id stable (`include/UndoPatch.h:74`, `include/UndoManager.h:12`) ; pas de notification (`:7`) ; opérations hors audio absentes (`src/Application.cpp:1961`) ; vidages silencieux (`src/AudioEngine.cpp:1817`) ; aucun journal sur disque (`include/StartupTrace.h:28` ne trace que le démarrage).
- Commandes distantes jamais enregistrées dans les actions (`src/Application.cpp:353` appelé uniquement par `MainWindow`).
- Alertes dans `Application` via les wrappers `AlertGate` (`:1187` synchrone, `:4170` asynchrone : avalées en headless, bloquantes en GUI sans capture) ; `AlertGate` global (`include/AlertGate.h:33`) ; 60 `runModalLoop` bruts et `FileChooser` synchrones hors de toute garde.
- Aucun verrou sur `docs` (`include/Application.h:864` : seul mutex = init audio) ; `runDestructiveEdit` compte sur le dialogue modal pour geler l'UI (`src/MainWindow.cpp:2021-2030`).
- Pas de modèle de job : threads détachés + `WorkProgressDialog` comme seule poignée d'annulation (`src/MainWindow.cpp:6255`, `:6539`).
- Deux registres de modèles ; assets audio.cpp vérifiés par taille seulement (`src/AudioCpp.cpp:205`) — **risque de sécurité** si une IA installe des binaires exécutables de 30 à 465 Mo.
- Résultats IA non retournés sous forme structurée (`src/Application.cpp:3951`).
- Zoom/vue/panneaux/sauvegarde/export PNG hors `Application` (`include/Application.h:120`, `src/MainWindow.cpp:2318`, `:2536`).
- Mesures : pas de RMS/peak réutilisable (`src/MainWindow.cpp:4358`) ; pas de détection de pics (« harmoniques » = multiples d'une ancre souris, `src/Panels.cpp:4188`) ; helpers 1/3 d'octave `static` file-scope (`:773-803`) ; trois conventions de niveaux (`include/Panels.h:634`, dB relatifs, dB-SPL décalé) avec pleine échelle par canal.
- Nom de tube global + serveur dans chaque instance (`include/SingleInstance.h:15`, `src/Main.cpp:840-844`).
- `runProcess` sans timeout (`src/PythonScripts.cpp:800`) ; stdin hérité (`:752`) ; sortie Python en page de code ANSI décodée UTF-8 (`:815`).
- Sans `MainWindow`, les réglages ne sont jamais persistés automatiquement (`src/MainWindow.cpp:1878`) ; écrire `app.settings` contourne les clamps (`src/Application.cpp:168-242`).

### Mineurs

Pas de sonde d'instance POSIX + délai 2 s (`src/SingleInstance.cpp:206`, `:178-179`) ; FIFO survivant à un crash (`juce_NamedPipe_posix.cpp:57-60`) ; `--test-transcribe` canal 0 non persisté (`src/Main.cpp:518`) ; stdout invisible pour l'exe GUI Windows (`src/CMakeLists.txt:2`) ; `resample` hors `Application` et hors undo (`src/Actions.cpp:464`) ; `ViewColourMap` mort (`include/MainWindow.h:503`) ; profondeur 64 (`include/UndoManager.h:10`) ; une action = N entrées sans nom ; `transcribe` mute le doc depuis le worker (`src/Application.cpp:3951`) ; `openFile` sans `errOut` (`:408`) ; `play()` bloquant sans timeout (`:4433`, surcharge `ensureAudioReady(int)` à `include/Application.h:778`) ; API IA conditionnelle à `SC_ENABLE_AI` (`:680`, `:745`) ; snapshots FFT non persistés (`include/Panels.h:678`) ; matrice spectrogramme sans format de données (`src/SpectralEngine.cpp:207`) ; `SC_MODELS_DIR` ignoré sur Windows (`src/AI/ModelManager.cpp:157`) ; `buildNsFailureDetails` défini dans le fichier UI (`src/MainWindow.cpp:6058`) ; progression audio.cpp = lignes de log brutes (`src/AudioCpp.cpp:586`) ; action `python` n'enregistre que le chemin du script (`src/MainWindow.cpp:260`) ; limite 2^31 échantillons dans l'échange WAV (`src/PythonScripts.cpp:637`) ; `findPython` sur le message thread à chaque run (`src/MainWindow.cpp:6334`) ; ctest basé sur une regex stdout (`tests/CMakeLists.txt`).

---

## 5. Architecture recommandée

### 5.1 Schéma

```
VS Code + Claude Code ──stdio MCP──▶ tools/mcp/spectrocraft_mcp (Python, FastMCP)
                                        │ lit <appData>/SpectroCraft/control/<pid>.json {port, token}
                                        │ TCP 127.0.0.1:<port>, NDJSON, 1re ligne = session.hello
                                        ▼
┌──────────────── SpectroCraft (GUI) ─────────────────────────────────────────────────────┐
│ ControlServer thread ─▶ ClientSession ──callAsync + PendingCall(timeout, abandoned)──▶  │
│                                              THREAD DE MESSAGES                         │
│   CommandRegistry::invoke  ◀── MainWindow menus ◀── applyActionStep ◀── --json (CLI)    │
│     flags → busy/modal → streamed? job : handler → recordActionStep → Journal → refresh │
│     LongRunning ─▶ JobManager (std::thread, 1 mutant max) ─progress─▶ events            │
│                      └─ commit ─▶ callAsync + WaitableEvent (Application.cpp:3602-3613) │
│   Application ─ FileDoc(id) ─ AudioEngine ─ UndoManager(onChanged) ─ ModelManager        │
│   Journal (anneau + JSONL, seq) ─▶ events.subscribe {sinceSeq} avec rejeu               │
└─────────────────────────────────────────────────────────────────────────────────────────┘
   même binaire : SpectroCraft --json req.ndjson --json-out res.ndjson  (headless, pompe)
   tests : testControlRegistry / testControlServer (port 0, ScModalWatchdog :27082)
```

### 5.2 Composants (nouveau dossier `src/control/`, `include/control/`)

**CommandRegistry** — `Spec { id, title, category, paramsSchema, resultSchema, flags, aliases, handler }`, drapeaux `Mutates | NeedsDoc | LongRunning | NeedsUi | Destructive | AiBuildOnly | WindowsOnly`. `invoke()` fait toujours, sur le thread de messages (assertion `JUCE_ASSERT_MESSAGE_THREAD`), dans cet ordre :

1. résolution de `doc` (id stable `FileDoc::id` ou chemin → index, jamais l'index brut). Les handlers hérités (`applyActionStep`, `app.selection`, `app.audioEngine()`) agissent sur le **document actif** (`activeDocIndex` `include/Application.h:54`, accesseurs `:262-263`). Règle : si `doc` désigne un document non actif, `invoke()` l'active pour la durée du handler (`activeDocIndex` + `syncActiveDoc()`, `Application.h:395`) puis **restaure** l'ancien actif et resynchronise ; l'UI voit un aller-retour de vue, signalé dans l'enveloppe par `"docSwitched":true`. Seul `doc.activate` change l'actif durablement. `selection.set` sur un doc non actif écrit dans la copie par document que `syncActiveDoc` maintient (`src/MainWindow.cpp:2840`), pas dans `app.selection` ; si cette copie n'inclut pas encore la sélection temporelle, le lot 0 l'y ajoute ;
2. drapeaux → `no_document`, `unsupported_build`, `unsupported_platform`, `needs_ui`, `confirm_required` ;
3. garde busy : `WorkProgressDialog::numActive() > 0` (`include/WorkProgress.h:86`), job mutant en cours, ou `juce::Component::getCurrentlyModalComponent() != nullptr` — évalué **ici**, sur le thread de messages, jamais depuis le thread réseau ;
4. **détour fichier streamé** : si `Mutates` et `audioEngine().fullyInMemory() == false` (`include/AudioEngine.h:45`), conversion en job qui fait `ensureEditable` sur un worker avec progression puis le handler (patron `runDestructiveEdit`, `src/MainWindow.cpp:1996-2040`) ;
5. `AlertGate::ScopedCapture` → handler → `recordActionStep` si enregistrement actif → `journal.append` (toujours) → `ui->refreshAfterEdit()` si `Mutates` et `UiBridge` ;
6. enveloppe enrichie de `undoDepth`, `redoDepth`, `redoDiscarded`, `contentVersion`, `journalSeq`.

Validation par handler avec un helper `expect(params, "db", Number, min, max)` → `invalid_params{path, expected}`. **Pas** de validateur JSON Schema générique côté C++ (dette refusée), mais `paramsSchema` n'est **pas** écrit à la main : chaque handler déclare ses paramètres une seule fois dans un tableau `ParamSpec { name, type, required, min, max, enumValues, doc }` ; ce même tableau alimente `expect()` (validation) **et** la génération d'un objet **JSON Schema draft 2020-12 restreint** (`type`, `properties`, `required`, `minimum`/`maximum`, `enum`, `items`, `description`, `additionalProperties:false`), publié par `app.describe` et passé tel quel comme `inputSchema` MCP (qui l'exige). Une seule source, donc pas de « sixième liste ». Test `testControlDescribe` : pour chaque commande, un `params` minimal généré depuis le schéma passe `expect()`, et tout `invalid_params` émis cite un `path` présent dans le schéma. Exemple de sortie en §6.2.

**Lot 0 : `step.run` générique** — une seule entrée qui enveloppe `ActionStep::fromJson` → `validateStepParams` (`src/Actions.cpp:346`) → `applyActionStep` (`:412`) → `recordActionStep` → `refreshAfterEdit`, avec `select {start,end}|"all"` et `channels[]` explicites. Les 20 types sont disponibles dès le lot 0 ; le lot 6 les éclate en `edit.*`/`effect.*`/`gen.*`/`ai.*` derrière des alias, et `applyActionStep` devient une façade de 5 lignes sur `invoke`.

**UiBridge** — interface abstraite `{ refreshAfterEdit, applyTimeWindow, zoomToSelection/Fit/OutFull, setPanelVisible, isPanelOpen, beginJobDialog, setJobProgress, endJobDialog, openFileAsync }` implémentée par `MainWindow` (`include/MainWindow.h:180-234` ; `openFile` `:1929` bascule sur `openFileWithProgress` au-delà de 200 Mo), `nullptr` en CLI/tests. Aucune commande n'appelle `menuItemSelected`, `runModalLoop`, `FileChooser` ou `showEditError`.

**ControlServer** — `juce::Thread "SC-Control"`, `StreamingSocket::createListener(0, "127.0.0.1")` + `getBoundPort()` ; un `ClientSession` par client (max 4), `waitUntilReady(true, 500)` pour voir `threadShouldExit()` (même 500 ms que `src/SingleInstance.cpp:97`), découpage sur `\n`, `juce::JSON::parse` ; un seul writer par socket (file sortante sous `CriticalSection`). Exécution d'une commande courte :

```cpp
auto st = std::make_shared<PendingCall>();   // WaitableEvent done; Outcome out; std::atomic<bool> abandoned{false};
if (!juce::MessageManager::callAsync ([st, req, this] { if (st->abandoned) return; st->out = dispatch (req); st->done.signal(); }))
    return error ("shutting_down");
if (!st->done.wait (req.timeoutMs /*30000*/)) { st->abandoned = true; return error ("timeout", {{"hint","a modal dialog is probably open"}}); }
```

(patron `src/Application.cpp:3602-3613` rendu sûr par le `shared_ptr` et le timeout ; l'attente infinie n'est jamais utilisée.) **Arrêt** : `shuttingDown = true; { StreamingSocket poke; poke.connect("127.0.0.1", boundPort, 500); } listener.close(); stopThread(2000);` — obligatoire parce que `close()` se connecte à `portNumber` resté à 0 (`juce_Socket.cpp:157-158`, `:613`). Démarrage dans `initialiseInternal` après `forwarder->startServer()` (`src/Main.cpp:845`) si réglage `control_enabled` (défaut 0) ou flag `--control` ; arrêt dans `shutdown()` **avant** `mainWindow.reset()` (`src/Main.cpp:856-857`). Le tube FileForward reste intact. Quand `--control` est présent, `initialiseInternal` **saute** `handleStartup` (`src/Main.cpp:825-832`) : le processus lancé par l'adaptateur démarre toujours sa propre instance, même avec `multi_instance=0` ; sinon il forwarderait ses fichiers à l'instance existante et quitterait sans écrire de fichier de découverte (`src/SingleInstance.cpp:191-213`). macOS : aucun entitlement n'est posé par le CMake aujourd'hui (pas d'App Sandbox), donc `createListener` fonctionne ; si l'app était un jour sandboxée/notarisée avec App Sandbox, il faudrait `com.apple.security.network.server`.

**Fichier de découverte** — `<userApplicationDataDirectory>/SpectroCraft/control/<pid>.json` = `{pid, port, token, version, startedAt, exe, protocol}`, 0600. Chemins réels : macOS `~/Library/SpectroCraft/control`, Linux `$XDG_CONFIG_HOME` ou `~/.config/SpectroCraft/control`, Windows `%APPDATA%\SpectroCraft\control`. Balayage des PID morts au démarrage. Le `0600` n'existe que sur POSIX ; sous Windows la protection est l'ACL par défaut du profil utilisateur (`%APPDATA%`), aucune ACL explicite n'est posée : le jeton est lisible par tout processus du même utilisateur, ce qui est le modèle de menace retenu (loopback + même compte, pas de défense intra-utilisateur).

**JobManager** — `Job { id "j-<n>", cmd, state, progress, status, result, error, atomic cancel, mutating, docId }` ; un `std::thread` par job (modèle `runNoiseSuppressAsync`, `src/MainWindow.cpp:6255`) ; **un seul job mutant à la fois** ; commit par le hop `callAsync` + `WaitableEvent` après alignement de `transcribe` (`:3951`) et de la branche par défaut de `noiseSuppress` (`:3562-3565`). Via `UiBridge`, un job mutant ouvre le même `WorkProgressDialog` non bloquant que l'UI, donc `numActive()`, `isBackgroundJobRunning` (`src/MainWindow.cpp:2445-2449`) et `systemRequestedQuit` (`src/Main.cpp:868`) le voient sans modification.

**Journal** — lot 0 : deux lignes dans `Application::recordActionStep` **avant** le `if (!actionRec.active …) return;` (`src/Application.cpp:355`) + dans `undoSteps/redoSteps` + dans `CommandRegistry::invoke` (toute commande API). **Portée réelle au lot 0** : `recordActionStep` n'est appelé que depuis 27 sites de `src/MainWindow.cpp` pour les 20 types d'étape (dialogues d'effets, Delete/Trim, Python, NS, transcription) ; `EditCut`/`EditPaste`/`EditPasteMix`/`EditPasteToNewChannels` (`src/MainWindow.cpp:3008-3010`, `:3020-3023`), marqueurs, sélections, dessins, tables, enregistrement et undo/redo à la souris (`:3006-3007`) n'y passent pas. Jusqu'au lot 5, `history.list.journal` voit donc **les éditions par dialogue et toutes les commandes API**, pas tout ce que Nicolas fait à la souris ; la pile undo (`states[]`) reste exhaustive pour l'audio, mais sans type/params pour ces entrées-là. Lot 5 : anneau 10 000 entrées + JSON Lines `<appData>/SpectroCraft/journal/session-<pid>-<date>.jsonl` (patron `StartupTrace::State::emit`, `include/StartupTrace.h:28-40`), `seq` monotone, `kind ∈ command|job|history|meta|doc|alert`, hooks `MarkerManager::onChange` (`include/MarkerManager.h:64`), setters `metaDirty` de `FileDoc`, vidages d'historique avec raison. `Patch` enrichi (`include/UndoPatch.h:53-75` : `seq, timestampMs, opId, params, source`) posé dans `recordReplace` via un contexte d'opération thread-local et complété via `lastRecorded()` (motif `src/Application.cpp:2038-2049`) ; `UndoManager::onChanged` dans `record/popUndo/popRedo/clear`.

**AlertGate::ScopedCapture** — `captureDepth` + liste `captured` dans `State` (`include/AlertGate.h:21-26`) ; `swallow()` empile quand `captureDepth > 0` ; `invoke()` renvoie `alerts[]`. Jamais `setHeadless(false)` après une commande.

**StateJson / Measures / ModelCatalog** — état app (getters atomiques `include/AudioEngine.h:44-98`), historique (sémantique `HistoryPanel::refresh`, jamais `oldData/newData`), méta (`FileDoc::buildMetaJson`) ; module `Measures` extrait de `FFTAnalysisView` (`computeRegionFrom` avec `windowType` en argument, `computeTimeCurveFrom`/`computeSpectrumCurveFrom` rendues static, helpers sortis du file-scope `:773-803`, `thirdOctBandsRef` inchangée) + `levels()` (RMS/peak/crête sur `readSamples`, thread-safe `include/AudioEngine.h:60-61`) + `peaks()` ; les mesures longues sont des jobs. `ModelCatalog` unifie `ModelManager` (`include/AI/ModelManager.h:35-57`) et les catalogues `sc::acpp` en lecture seule.

**Mode CLI `--json`** — `--json <fichier|->` `[--json-out <f>]` `[--use-user-settings]` `[--list-commands [--markdown]]` sur le squelette de `--run-action` (`src/Main.cpp:293-330`), `setHeadless(true)`, `SC_NO_AUDIO_DEVICE` ; chaque job attendu en pompant `MessageManager::runDispatchLoopUntil` (patron `scPumpUntil`, `tests/UnitTests.cpp:207-216`) et inliné ; codes 0/1/2 ; branche `else if (tok.startsWith("--"))` → `unknown flag`, code 2. Lecture de stdin **ligne à ligne** (streaming, jamais « tout lire puis exécuter ») sur le thread de messages entre deux pompes : chaque ligne est exécutée avant de lire la suivante et sa réponse est écrite puis vidée (`flush`) immédiatement, ce qui permet un dialogue depuis un sous-processus Python ; ligne > 1 Mio → `invalid_params`. Une commande `LongRunning` est pompée jusqu'à `timeoutMs` (défaut 30 s, `0` = illimité), puis annulée (`job.cancel`) et rapportée `timeout` ; `continueOnError` décide de la suite. Windows : l'exe est `juce_add_gui_app` (`src/CMakeLists.txt:2`), donc sans console ; les handles stdin/stdout **hérités** d'un parent qui fournit des `PIPE` fonctionnent (`GetStdHandle` les voit), c'est ce que fait `sc_headless` ; lancé depuis un terminal interactif, rien ne s'affiche — comportement à affirmer par un test `testControlJsonCli` exécuté dans le job `windows-ai` du CI.

### 5.3 Threading et sécurité, en résumé

- Tout handler court s'exécute sur le thread de messages ; un handler n'attend jamais un worker (la garantie de `src/Application.cpp:3608-3609` ne tient que dans un sens). Tout ce qui attend va dans un job.
- Le thread réseau ne touche jamais `Application` ni `docs` (aucun verrou, `include/Application.h:53`) ; il ne fait que parser, poster et attendre avec timeout.
- Loopback seulement, jeton `Uuid` par instance, `session.hello` obligatoire, 3 échecs → pause 2 s ; `confirm:true` exigé pour `Destructive` (save écrasant, close non sauvé, python hors `scriptFolder()`) ; assets audio.cpp listés mais `installable:false` tant qu'ils n'ont pas de SHA-256 ; le jeton n'est jamais journalisé.
- Opt-in persistant « Window > Allow AI control (localhost) » calqué sur `WindowAllowMultiple` (`src/MainWindow.cpp:2796`) + indicateur dans la barre d'état quand un client est connecté.
- **Périmètre de chemins** : `doc.open`, `doc.save/saveAs`, `export.*`, `markers.importCsv/exportCsv`, `python.run {script}` n'acceptent que des chemins absolus **sous une liste de racines autorisées** (réglage `control_roots`, défaut : dossier personnel de l'utilisateur + `scriptFolder()` (`src/PythonScripts.cpp:519-523`) + dossier temporaire), résolus après `File::getLinkedTarget()` ; hors périmètre → `confirm_required{reason:"outside_roots"}`, jamais silencieux ; cible existante en écriture → `confirm`. Le dossier `control/` (jetons) et `SpectroCraft.recent` sont refusés sans `confirm` possible.
- **Limites** : ligne NDJSON ≤ 1 Mio (socket et `--json`), `params` ≤ 256 Kio après parse, 64 requêtes en vol par client, file d'événements bornée (1000 ; au-delà, les plus anciens sont perdus et un `events.dropped{count}` est émis), 4 clients, pause 2 s après 3 `unauthorized`.
- **Plusieurs clients** (deux fenêtres VS Code) : les commandes sont sérialisées par le thread de messages ; un seul job mutant à la fois, quel que soit le client ; `history.undo` agit sur la pile du document, donc un client peut annuler l'édition d'un autre — voulu (une app, une pile), mais chaque entrée de journal porte `source:"api:<client>"` (champ `client` de `session.hello`) et `history.changed` est diffusé à tous les abonnés, pour que l'adaptateur puisse le signaler à Claude.
- **Contenu du journal** : métadonnées seulement (ids, types, `params`, chemins, `segmentCount`, durées) ; jamais le texte de transcription ni les valeurs de tables (contenu potentiellement sensible) sauf réglage `journal_verbose` ; le jeton n'y figure jamais.

### 5.4 Pourquoi pas les deux autres architectures

**A — Pont minimal (socket + dispatcher `switch` sur `applyActionStep`, MCP Python à la main)** : le plus rapide (8,5 j) mais il ajoute une **sixième** liste de types maintenue à la main à côté de `actionTypes` / `isKnownActionType` / `validateStepParams` / `applyActionStep` / `continueReplay`, c'est-à-dire le mécanisme exact qui a produit le bug `envelope` ; menus et dispatcher restent deux chemins à tester ; il adressait les documents par index et, en mode `--json`, ne pompait pas la boucle de messages (les jobs ne se terminaient jamais).

**C — Bus de commandes complet (registre + validateur JSON Schema + journal + serveur)** : équivalent à B en qualité et en découvrabilité, mais 28 jours, avec le serveur de contrôle au lot 3 (~16 j avant de piloter l'app ouverte, à rebours du besoin principal), un validateur JSON Schema maison (~150 lignes sans réutilisation JUCE) dès le lot 0, et une incohérence interne (résolution de `docId` sur le thread réseau, qui lit `docs` sans verrou).

---

## 6. Protocole JSON

### 6.1 Enveloppe (NDJSON ; identique sur le socket et en mode `--json`)

```
Requête  : {"id":"r12","cmd":"<domaine.commande>","params":{...},"doc":<docId|chemin>,"timeoutMs":30000}
Réponse  : {"id":"r12","ok":true,"result":{...},"alerts":[],
            "undoDepth":5,"redoDepth":0,"redoDiscarded":0,"contentVersion":56,"journalSeq":413,"durationMs":21}
Erreur   : {"id":"r12","ok":false,"error":{"code":"<code>","message":"...","details":{...}}}
Événement: {"event":"<nom>","seq":414,"ts":"2026-11-05T14:21:09.002Z","data":{...}}
Job      : {"ok":true,"result":{"job":"j-7","state":"running"}}   (en --json : attendu et inliné)
```

`doc` absent = document actif. Les champs d'état de l'enveloppe ne sont présents que pour les commandes `Mutates`. Codes fermés : `unauthorized`, `unknown_cmd`, `invalid_params` (`details.path`, `details.expected`), `no_document`, `unknown_doc`, `busy` (`details.job` ou `details.modalActive`), `timeout`, `cancelled`, `confirm_required` (`details.hint`), `needs_ui`, `unsupported_build`, `unsupported_platform`, `model_not_installed`, `job_not_found`, `shutting_down`, `failed`. Unités : secondes, canaux 0-based, niveaux en dB avec `unit` explicite (`dBFS` ou `dB-SPL`/`dBV` via `ScaleCalibration`).

### 6.2 État de l'app

```json
{"id":"1","cmd":"session.hello","params":{"token":"7f3c…","client":"spectrocraft-mcp/0.1","subscribe":["job","history","doc"],"sinceSeq":0}}
{"id":"1","ok":true,"result":{"app":"SpectroCraft","version":"1.0.72","pid":51233,"protocol":1,"platform":"macos","capabilities":{"ai":true,"audiocpp":false,"python":true,"ui":true},"commands":48,"journalSeq":413}}

{"id":"3","cmd":"app.state","params":{}}
{"id":"3","ok":true,"result":{"docs":[{"id":7,"path":"/Users/nicolas/audio/recording_a.wav","name":"recording_a.wav","durationSec":12.345,"channels":2,"sampleRate":48000,"unsaved":true,"fullyInMemory":true,"undoDepth":3,"redoDepth":0,"contentVersion":41}],"activeDoc":7,"selection":{"start":1.0,"end":2.5,"hasSpectral":false},"playhead":1.0,"visibleChannels":[0,1],"view":{"t0":0.0,"t1":12.345,"fMin":20,"fMax":24000},"transport":{"playing":false,"paused":false,"looping":false,"audioReady":true},"recording":{"active":false},"ai":{"device":"auto","resolved":"CPU","lastNsModel":"deepfilternet2","lastTranscribeModel":"whisper-base","autoDownload":true},"busy":false,"modalActive":false,"jobs":[]}}
```

`version` vient de `generated/Version.h` / `SC_VERSION.txt` (patch auto-incrémenté par le CMake, `CMakeLists.txt:53-65`, `VERSION.txt` = `1.0.72` au 2026-11-05), **pas** de `getApplicationVersion()` qui renvoie `"1.0.0"` en dur (`src/Main.cpp:132`) ; l'adaptateur compare `protocol`, jamais `version`.

`app.describe {category, withSchemas}` renvoie `{commands:[{id, aliases, title, category, flags, params:<schema>, result:<schema>}]}` — source des outils MCP dynamiques. Le schéma est généré depuis les `ParamSpec` des handlers (§5.2), au format JSON Schema restreint :

```json
{"id":"2","cmd":"app.describe","params":{"category":"effect","withSchemas":true}}
{"id":"2","ok":true,"result":{"protocol":1,"commands":[
 {"id":"effect.changeGain","aliases":["changeGain"],"title":"Change gain","category":"effect","flags":["Mutates","NeedsDoc"],
  "params":{"type":"object","properties":{"db":{"type":"number","minimum":-96,"maximum":96,"description":"Gain in dB"},"select":{"oneOf":[{"type":"string","enum":["all"]},{"type":"object","properties":{"start":{"type":"number","minimum":0},"end":{"type":"number","minimum":0}},"required":["start","end"]}]},"channels":{"type":"array","items":{"type":"integer","minimum":0}}},"required":["db"],"additionalProperties":false},
  "result":{"type":"object","properties":{"patchSeq":{"type":"integer"},"description":{"type":"string"}}}}]}}
```

### 6.3 Historique

```json
{"id":"5","cmd":"history.list","params":{"includeJournal":true,"since":400},"doc":7}
{"id":"5","ok":true,"result":{"docId":7,"currentState":3,"maxLevels":64,
 "states":[
  {"state":0,"label":"Loaded recording_a.wav"},
  {"state":1,"patchSeq":101,"ts":"2026-11-05T14:18:02.410Z","opId":"effect.changeGain","params":{"db":6.0},"description":"Changed gain by +6.00 dB","region":{"startSec":1.0,"endSec":2.5},"lengthDelta":0,"channelsChanged":false,"rateChanged":false,"sideState":false,"dataBytes":576000,"source":"menu","future":false},
  {"state":2,"patchSeq":102,"opId":"edit.delete","description":"Deleted samples","lengthDelta":-24000,"sideState":true,"markersBefore":3,"markersAfter":2,"source":"api:claude-code","future":false},
  {"state":3,"patchSeq":103,"opId":"ai.noiseSuppress","params":{"modelId":"deepfilternet2","device":"CPU"},"description":"Noise suppressed (deepfilternet2)","future":false}],
 "journal":[
  {"seq":409,"ts":"…","kind":"history","doc":5,"what":"cleared","reason":"resample"},
  {"seq":412,"ts":"…","kind":"meta","doc":7,"what":"marker.add","data":{"name":"door_slam","start":4.1}}],
 "recordedActions":[{"name":"Clean takeoff","steps":[{"type":"normalize","params":{}}]}]}}

{"id":"6","cmd":"history.jumpTo","params":{"state":1},"doc":7}
{"id":"6","ok":true,"result":{"currentState":1,"undone":2,"redone":0},"undoDepth":1,"redoDepth":2}

{"id":"6a","cmd":"history.undo","params":{"steps":1},"doc":7}
{"id":"6a","ok":true,"result":{"currentState":0,"undone":1,"description":"Changed gain by +6.00 dB"},"undoDepth":0,"redoDepth":3,"redoDiscarded":0,"contentVersion":57,"journalSeq":415}
{"id":"6b","cmd":"history.redo","params":{},"doc":7}
{"id":"6b","ok":true,"result":{"currentState":1,"redone":1},"undoDepth":1,"redoDepth":2,"redoDiscarded":0,"contentVersion":58,"journalSeq":416}
{"id":"6b","ok":false,"error":{"code":"failed","message":"Redo failed (not enough memory?); history cleared","details":{"historyCleared":true,"see":"src/Application.cpp:4224"}}}

{"id":"6c","cmd":"journal.read","params":{"sinceSeq":400,"kinds":["command","history"],"limit":200}}
{"id":"6c","ok":true,"result":{"entries":[
  {"seq":401,"ts":"2026-11-05T14:18:02.410Z","kind":"command","cmd":"step.run","doc":7,"source":"api:claude-code","ok":true,"durationMs":41,"params":{"step":{"type":"changeGain","params":{"db":-3.0}}}},
  {"seq":409,"ts":"…","kind":"history","doc":5,"what":"cleared","reason":"resample"}],
 "nextSeq":417,"file":"/Users/nicolas/Library/SpectroCraft/journal/session-51233-2026-11-05.jsonl"}}
```

`history.undo/redo` = `Application::undoSteps/redoSteps` (`src/Application.cpp:4175`) ; en cas d'échec d'`applyPatch` la pile est vidée (`:4192`, `:4224`) et la commande le dit (`historyCleared`) au lieu de l'alerte asynchrone (`:4170`, capturée par `ScopedCapture`). `journal.read` lit l'anneau (lot 0 : mémoire seulement, `file:null`) puis le JSONL (lot 5).

Avant le lot 5, `opId/params/ts/source` viennent du journal de session joint par index undo ; après, du `Patch` enrichi. `history.jumpTo` n'exige aucun `confirm` : un saut ne détruit jamais la pile redo (`include/UndoManager.h:35-42`) ; c'est la première édition suivante qui la vide, signalée par `redoDiscarded`.

### 6.4 Exécuter une commande / une étape

```json
{"id":"4","cmd":"step.run","params":{"select":{"start":1.25,"end":3.8},"channels":[0],"step":{"type":"changeGain","params":{"db":-3.0}}},"doc":7}
{"id":"4","ok":true,"result":{"applied":"changeGain","patchSeq":18,"description":"Changed gain by -3.00 dB","elapsedMs":41},"alerts":[],"undoDepth":4,"redoDepth":0,"redoDiscarded":0,"contentVersion":42,"journalSeq":414}

{"id":"4","ok":false,"error":{"code":"invalid_params","message":"changeGain: missing numeric param \"db\"","details":{"path":"step.params.db","expected":"number"}}}
```

Sur un fichier streamé (`fullyInMemory:false`) la même requête renvoie `{"job":"j-3"}`. Si `doc` n'est pas le document actif, l'app bascule dessus le temps de la commande puis revient (§5.2 étape 1) et l'enveloppe porte `"docSwitched":true`.

```json
{"id":"4b","cmd":"steps.run","params":{"select":"all","steps":[{"type":"normalize","params":{}},{"type":"fadeOut","params":{}}],"stopOnError":true},"doc":7}
{"id":"4b","ok":true,"result":{"applied":2,"results":[{"type":"normalize","ok":true,"patchSeq":19},{"type":"fadeOut","ok":true,"patchSeq":20}]},"undoDepth":6,"redoDepth":0,"redoDiscarded":0,"contentVersion":44,"journalSeq":418}

{"id":"4c","cmd":"step.validate","params":{"step":{"type":"envelop","params":{}}}}
{"id":"4c","ok":false,"error":{"code":"invalid_params","message":"unknown step type \"envelop\"","details":{"path":"step.type","expected":"one of actionTypes (include/Actions.h:77-96)"}}}
{"id":"4d","cmd":"step.validate","params":{"step":{"type":"changeGain","params":{"db":6.0}}}}
{"id":"4d","ok":true,"result":{"valid":true,"type":"changeGain","mutates":true,"longRunning":false}}
```

`steps.run` = une entrée d'historique par étape (comme `continueReplay`), `stopOnError:false` continue et rapporte chaque erreur dans `results[]` ; `step.validate` = `ActionStep::fromJson` + `validateStepParams` (`src/Actions.cpp:346`) sans jamais toucher au document.

```json
{"id":"7","cmd":"doc.open","params":{"path":"/Users/nicolas/audio/recording_b.wav","activate":true}}
{"id":"7","ok":true,"result":{"doc":8,"durationSec":30.0,"channels":4,"sampleRate":48000,"fullyInMemory":true}}
{"id":"8","cmd":"selection.set","params":{"start":2.0,"end":3.5,"channels":[0,1]},"doc":8}
{"id":"9","cmd":"doc.save","params":{"path":"/Users/nicolas/audio/recording_b_clean.wav","meta":"sidecar"},"doc":8}
{"id":"9","ok":false,"error":{"code":"confirm_required","message":"Target exists; resend with params.confirm=true","details":{"hint":"pass confirm:true, or choose a new path"}}}
{"id":"10","cmd":"doc.close","params":{},"doc":7}
{"id":"10","ok":false,"error":{"code":"confirm_required","message":"Document has unsaved changes","details":{"unsaved":true,"hint":"call doc.save first or pass confirm:true"}}}
```

`doc.open` > 200 Mo (seuil `src/MainWindow.cpp:1929`) → `{"job":"j-12"}`.

```json
{"id":"7a","cmd":"doc.list","params":{}}
{"id":"7a","ok":true,"result":{"active":8,"docs":[{"id":7,"path":"/Users/nicolas/audio/recording_a.wav","unsaved":true,"undoDepth":3,"durationSec":12.345},{"id":8,"path":"/Users/nicolas/audio/recording_b.wav","unsaved":false,"undoDepth":0,"durationSec":30.0}]}}

{"id":"9b","cmd":"doc.save","params":{"path":"/Users/nicolas/audio/recording_b_clean.wav","meta":"sidecar","confirm":true},"doc":8}
{"id":"9b","ok":true,"result":{"path":"/Users/nicolas/audio/recording_b_clean.wav","bytes":5760044,"meta":{"mode":"sidecar","written":true},"unsaved":false,"durationMs":180}}

{"id":"11","cmd":"doc.meta","params":{"sections":["markers","selections","calibration","transcription"]},"doc":8}
{"id":"11","ok":true,"result":{"mode":"sidecar","bundle":{"markers":[…],"selections":[…],"calibration":{…},"transcription":{…}}}}

{"id":"24","cmd":"view.set","params":{"t0":1.5,"t1":4.0},"doc":8}
{"id":"24","ok":true,"result":{"t0":1.5,"t1":4.0,"visibleChannels":[0,1,2,3]}}
{"id":"25","cmd":"view.zoom","params":{"to":"selection"}}
{"id":"25","ok":true,"result":{"t0":2.0,"t1":3.5}}
{"id":"26","cmd":"view.channels","params":{"visible":[0]}}
{"id":"26","ok":true,"result":{"visibleChannels":[0]}}

{"id":"27","cmd":"transport.play","params":{"from":2.0,"loop":false}}
{"id":"27","ok":true,"result":{"playing":true,"position":2.0,"audioReady":true}}
{"id":"28","cmd":"transport.stop","params":{}}
{"id":"28","ok":true,"result":{"playing":false,"position":2.73}}
{"id":"27","ok":false,"error":{"code":"failed","message":"No audio device (SC_NO_AUDIO_DEVICE or init timeout)","details":{"audioReady":false}}}

{"id":"29","cmd":"markers.add","params":{"name":"door_slam","start":4.1,"end":4.35},"doc":8}
{"id":"29","ok":true,"result":{"index":3,"count":4}}
{"id":"30","cmd":"markers.list","params":{},"doc":8}
{"id":"30","ok":true,"result":{"markers":[{"index":0,"name":"start","start":0.0,"end":0.0},{"index":3,"name":"door_slam","start":4.1,"end":4.35}]}}
{"id":"31","cmd":"markers.exportCsv","params":{"path":"/Users/nicolas/audio/recording_b_markers.csv"},"doc":8}
{"id":"31","ok":true,"result":{"path":"/Users/nicolas/audio/recording_b_markers.csv","count":4}}

{"id":"32","cmd":"settings.get","params":{"keys":["ai_device","multi_instance","control_enabled"]}}
{"id":"32","ok":true,"result":{"values":{"ai_device":"auto","multi_instance":1,"control_enabled":1},"file":"/Users/nicolas/Library/Preferences/SpectroCraft.recent"}}
{"id":"33","cmd":"settings.set","params":{"values":{"ai_device":"CPU","fft_size":99999}}}
{"id":"33","ok":true,"result":{"applied":{"ai_device":"CPU","fft_size":65536},"clamped":["fft_size"],"persisted":true}}

{"id":"34","cmd":"python.run","params":{"script":"/Users/nicolas/Library/SpectroCraft/scripts/hp_filter.py","timeoutMs":120000},"doc":8}
{"id":"34","ok":true,"result":{"job":"j-20"}}
{"event":"job.done","seq":480,"data":{"job":"j-20","ok":true,"result":{"patchSeq":21,"stdoutTail":"…","scriptSha256":"…","resultJson":null}}}
```

`doc.meta` = sortie de `FileDoc::buildMetaJson` (`include/FileDoc.h:173`), filtrée par `sections`. `view.*` = `viewStart/viewEnd` + `ensureTimeRangeVisible` (`include/Application.h:120`, `:483`) et `visibleChannels` (`:260`) ; `view.zoom {vertical}` et `view.show {waveform|spectral}` sont `NeedsUi` (membres de `MainWindow`) → `needs_ui` en CLI. `transport.*` = `play()/stop()/pause()/toggleLoop()` avec `ensureAudioReady(2000)` (`include/Application.h:778`) au lieu de l'attente sans timeout (`src/Application.cpp:4433`). `markers.*` = `MarkerManager` (champs = ceux de `saveCsv`, `src/MarkerManager.cpp:745` ; `importCsv` = `loadCsv` `:757`), hors pile undo (§3 #10), journalisés dès le lot 5 via `onChange` (`include/MarkerManager.h:64`). `settings.*` passe par les clamps de `src/Application.cpp:168-242` (jamais `app.settings` à nu), persiste par `saveSettings` (`:350-380`, réécriture complète de `.recent`), clé inconnue → `invalid_params`. `python.run` = `runPythonScript` (`src/Application.cpp:3629`) en job avec timeout nouveau ; `confirm` exigé hors `scriptFolder()` ; `resultJson` (marqueurs/mesures renvoyés par le script) n'existe qu'à partir du lot 7.

### 6.5 Modèles IA : lister / installer / exécuter (avec backend)

```json
{"id":"12","cmd":"ai.models.list","params":{}}
{"id":"12","ok":true,"result":{"modelsRoot":"/Users/nicolas/Library/SpectroCraft/models","models":[
 {"id":"deepfilternet2","engine":"openvino","family":"noiseSuppress","displayName":"DeepFilterNet 2 (recommended)","installed":true,"verified":true,"bytesOnDisk":9700000,"path":"…/models/deepfilternet2","checksum":"sha256","backends":["CPU"]},
 {"id":"whisper-base","engine":"whisper","family":"transcribe","installed":false,"sizeBytes":147951465,"missing":["ggml-base.bin"],"checksum":"sha256","backends":["CPU"]},
 {"id":"sam-audio-base-q8_0","engine":"audiocpp","family":"stems","installed":false,"sizeBytes":2400000000,"checksum":"size-only","platforms":["windows-x64"],"installable":false,"reason":"no checksum (AudioCpp.cpp:205)"}]}}

{"id":"13","cmd":"ai.models.install","params":{"modelId":"whisper-base"}}
{"id":"13","ok":true,"result":{"job":"j-14","sizeBytes":147951465}}
{"event":"job.progress","seq":420,"data":{"job":"j-14","progress":0.63,"status":"downloading ggml-base.bin 93/148 MB"}}
{"event":"job.done","seq":431,"data":{"job":"j-14","ok":true,"result":{"modelId":"whisper-base","installed":true,"verified":true}}}

{"id":"15","cmd":"ai.devices","params":{}}
{"id":"15","ok":true,"result":{"openvino":{"available":true,"devices":["CPU"],"preference":"auto","resolved":"CPU","note":"only the CPU plugin ships"},"whisper":{"devices":["CPU"]},"audiocpp":{"available":false,"reason":"unsupported_platform","installedVariant":null}}}

{"id":"16","cmd":"ai.noiseSuppress","params":{"modelId":"deepfilternet2","device":"auto","attenLimDb":12,"df3PostFilter":false,"copyNoise":false,"autoDownload":true},"doc":8}
{"id":"16","ok":true,"result":{"job":"j-15"}}
{"event":"job.done","seq":450,"data":{"job":"j-15","ok":true,"result":{"patchSeq":19,"deviceUsed":"CPU","compileMs":8400,"inferMs":2210,"region":{"start":2.0,"end":3.5}},"undoDepth":2,"contentVersion":44}}

{"id":"17","cmd":"ai.transcribe","params":{"modelId":"whisper-base","language":"fr","translate":false,"fast":false,"channels":[0],"storeInDoc":true},"doc":8}
{"event":"job.done","seq":460,"data":{"job":"j-16","ok":true,"result":{"language":"fr","segments":[{"start":0.42,"end":2.10,"text":"Tour, Alpha 21, prêt au point d'arrêt."}],"segmentCount":1,"storedInDoc":true}}}
```

```json
{"id":"36","cmd":"ai.setDevice","params":{"engine":"openvino","device":"GPU"}}
{"id":"36","ok":false,"error":{"code":"invalid_params","message":"device GPU is not available for openvino","details":{"path":"device","expected":"one of [auto, CPU]"}}}
{"id":"37","cmd":"ai.setDevice","params":{"engine":"openvino","device":"CPU","persist":true}}
{"id":"37","ok":true,"result":{"engine":"openvino","device":"CPU","resolved":"CPU","persisted":true}}

{"id":"38","cmd":"ai.models.verify","params":{"modelId":"whisper-base"}}
{"id":"38","ok":true,"result":{"job":"j-17"}}
{"event":"job.done","seq":436,"data":{"job":"j-17","ok":true,"result":{"modelId":"whisper-base","verified":true,"files":[{"name":"ggml-base.bin","bytes":147951465,"sha256Ok":true}]}}}
{"id":"39","cmd":"ai.models.remove","params":{"modelId":"whisper-base"}}
{"id":"39","ok":false,"error":{"code":"confirm_required","message":"Removes 148 MB under <modelsRoot>/whisper-base","details":{"hint":"pass confirm:true"}}}

{"id":"35","cmd":"transcription.get","params":{"channel":0},"doc":8}
{"id":"35","ok":true,"result":{"source":"sctt","language":"fr","model":"whisper-base","segments":[{"start":0.42,"end":2.10,"text":"Tour, Alpha 21, prêt au point d'arrêt."}]}}
```

- `ai.devices` : la liste OpenVINO exige `OvRuntime::init` (`src/AI/OvRuntime.cpp:89`, mutex `:95`, `ov::Core` `:126`), coûteux la première fois (chargement de la bibliothèque) ; `devices()` (`:148`) appelle `get_available_devices` (`:152`) et une liste vide signifie « init échoué » (`:34`). Par défaut `ai.devices {init:false}` renvoie `"openvino":{"initialised":false,"devices":null,"preference":"auto"}` sans rien charger ; `init:true` lance l'initialisation dans un **job** et renvoie la liste réelle. Whisper : `use_gpu=false` figé (`src/AI/WhisperTranscribe.cpp:91`) → `["CPU"]`. audio.cpp : `cpu|cuda|vulkan` d'après `variant.txt` (`src/StemSeparation.cpp:497-503`), `unsupported_platform` hors Windows.
- `ai.setDevice {engine, device, persist}` : pour `openvino` écrit `aiDevicePref` (`include/Application.h:385`, partagé NS + transcription, lu `src/Application.cpp:145`, sauvé `:379`) après vérification contre la liste réelle (`resolveAiDevice`, `src/AI/OvRuntime.cpp:24`) ; `persist:true` appelle `saveSettings` (réécriture complète de `.recent`, §2.2) ; sans `persist`, la préférence vaut pour la session. `engine:"audiocpp"` → `--backend` transmis au CLI, mémorisé à part.
- `ai.models.verify` : job qui recalcule le SHA-256 (`src/AI/Sha256.cpp`) de chaque fichier de `dirFor()` et complète `isInstalled` (présence seule). `ai.models.remove` : `confirm` obligatoire, `busy` si un job utilise le modèle, suppression récursive de `dirFor()` uniquement sous `modelsRoot`.
- `transcription.get` : même forme que `scttToJson` (`src/ScttChunk.cpp:57`), `source ∈ sctt|sidecar|memory` selon l'origine (`src/Application.cpp:4131`, `:4137`) ; `text` n'est jamais journalisé (§5.3).

### 6.6 Mesurer une région

```json
{"id":"18","cmd":"measure.levels","params":{"start":2.0,"end":3.5,"channels":[0,1]},"doc":8}
{"id":"18","ok":true,"result":{"region":{"start":2.0,"end":3.5,"samples":72000},"unit":"dBFS","calibration":{"unit":"Pa","fullScale":[1.0,1.0],"levelOffsetDb":[94.0,94.0]},"channels":[{"ch":0,"rmsDb":-18.3,"peakDb":-6.1,"peakAtSec":2.313,"crestDb":12.2,"dcOffset":0.0003,"clipped":0,"rmsAbsDb":75.7,"absUnit":"dB-SPL"}]}}

{"id":"19","cmd":"measure.thirdOctave","params":{"start":5.0,"end":8.0,"channel":0,"mode":"reference","order":40},"doc":8}
{"id":"19","ok":true,"result":{"mode":"reference","window":"hanning-65536","order":40,"unit":"dB-SPL","levelOffsetDb":94.0,"framesUsed":1024,"framesCap":2048,"bands":[{"fcHz":50.1,"relDb":-62.3,"absDb":31.7},{"fcHz":19953,"relDb":-71.6,"absDb":22.4}],"emptyBandValue":-999}}

{"id":"20","cmd":"measure.spectrum","params":{"start":2.0,"end":3.5,"channels":[0],"fftSize":4096,"window":"hann","peaks":{"maxCount":10,"minProminenceDb":6}},"doc":8}
{"id":"20","ok":true,"result":{"analysis":{"fftSize":4096,"window":"hann","enbwBins":1.5,"framesAveraged":64,"framesCapped":false,"binHz":11.72,"levelConvention":"RMS single-sided, dB re full scale"},"channels":[{"ch":0,"relDb":[-96.1],"absDb":[-2.1],"absUnit":"dB-SPL"}],"peaks":[{"ch":0,"freqHz":1203.4,"relDb":-21.5,"absDb":72.5,"prominenceDb":18.2}]}}

{"id":"21","cmd":"measure.harmonics","params":{"start":2.0,"end":3.5,"channel":0,"anchorHz":1203.4,"n":1,"maxK":10},"doc":8}
{"id":"21","ok":true,"result":{"fundamentalHz":1203.4,"unit":"dB-SPL","harmonics":[{"k":1,"freqHz":1203.4,"relDb":-21.5,"absDb":72.5},{"k":2,"freqHz":2406.8,"relDb":-33.0,"absDb":61.0}]}}
```

`measure.thirdOctave` = `thirdOctBandsRef` (`src/Panels.cpp:648`) + `ScaleCalibration::levelOffsetDb(ch)` ; référence de test ≈ 91 dB-SPL (`tests/UnitTests.cpp:2688-2775`). `mode:"iec"` = 26 bandes non calibrées (`toThirdOctBands`, `:803`). Les mesures longues sont des jobs.

```json
{"id":"40","cmd":"measure.curve","params":{"start":2.0,"end":3.5,"channel":0,"kind":"rms","windowMs":50,"hopMs":10},"doc":8}
{"id":"40","ok":true,"result":{"kind":"rms","unit":"dBFS","t0":2.0,"hopSec":0.01,"count":150,"values":[-18.9,-18.2,-17.6]}}

{"id":"41","cmd":"measure.probe","params":{"t":2.31,"f":1200.0,"channel":0},"doc":8}
{"id":"41","ok":true,"result":{"t":2.31,"f":1200.0,"frameSec":2.304,"binHz":11.72,"magDb":-21.7,"unit":"dBFS"}}

{"id":"42","cmd":"measure.spectrogramData","params":{"start":2.0,"end":3.5,"channel":0,"fftSize":2048,"hop":512,"fMax":8000,"format":"auto"},"doc":8}
{"id":"42","ok":true,"result":{"job":"j-21"}}
{"event":"job.done","seq":482,"data":{"job":"j-21","ok":true,"result":{"frames":141,"bins":342,"binHz":23.44,"hopSec":0.01067,"unit":"dBFS","inline":false,"format":"f32","path":"/tmp/sc-spec-j21.f32","layout":"frames x bins, float32 little-endian, C order"}}}

{"id":"43","cmd":"export.spectrogram","params":{"path":"/Users/nicolas/audio/recording_b_spec.png","start":0,"end":30,"width":1920,"height":600,"overlays":{"axes":true,"markers":true,"selections":false,"drawings":false}},"doc":8}
{"id":"43","ok":true,"result":{"job":"j-22"}}
{"event":"job.done","seq":490,"data":{"job":"j-22","ok":true,"result":{"path":"/Users/nicolas/audio/recording_b_spec.png","width":1920,"height":600,"bytes":412000,"overlays":{"axes":true,"markers":true}}}}
```

`measure.curve` = `computeTimeCurveFrom`/`computeSpectrumCurveFrom` rendues static (`include/Panels.h:796`, `:803`) ; `measure.probe` = `SpectralEngine::probeMagnitude` (`src/SpectralEngine.cpp:460`) ; `measure.spectrogramData` = `computeTile` (`:207`) par tuiles, en job. **Gros volumes** : au-delà de 1 M valeurs le résultat est écrit dans un fichier (`inline:false`, `path`) ; formats `csv` (écriture ligne à ligne, aucun writer à réutiliser : le TSV existant vise le presse-papiers, `src/Panels.cpp:4676`) et `f32` brut (lisible par `numpy.fromfile(...).reshape(frames, bins)`). **Aucun writer `.npy` n'existe dans le dépôt** ; l'ajouter (en-tête ASCII v1.0 + float32, ~60 lignes) coûte ~0,25 j, compté dans le lot 4 mais optionnel puisque `f32` suffit. `export.spectrogram` : sans UI, `renderToPng` (`include/SpectralEngine.h:129`) sans overlays ; avec overlays, `preparePngExport` via `UiBridge` (`src/MainWindow.cpp:2536`) → `needs_ui` en CLI si `overlays` est demandé.

### 6.7 Progression / annulation

```json
{"id":"14","cmd":"job.get","params":{"job":"j-14"}}
{"id":"14","ok":true,"result":{"job":"j-14","cmd":"ai.models.install","state":"done","progress":1.0,"result":{"modelId":"whisper-base","installed":true}}}
{"id":"22","cmd":"job.cancel","params":{"job":"j-15"}}
{"id":"22","ok":true,"result":{"job":"j-15","state":"cancelling"}}
{"event":"job.done","seq":455,"data":{"job":"j-15","ok":false,"error":{"code":"cancelled","message":"cancelled by client"}}}

{"id":"23","cmd":"events.subscribe","params":{"kinds":["job","history","doc","meta"],"sinceSeq":470}}
{"id":"23","ok":true,"result":{"subscribed":true,"replayed":3}}
{"event":"history.changed","seq":474,"data":{"doc":8,"undoDepth":4,"redoDepth":0,"patchSeq":124,"opId":"edit.silence","source":"menu"}}
{"event":"doc.changed","seq":475,"data":{"doc":8,"kind":"selection"}}
```

Annulation : `ensureInstalled` (ProgressFn → false), whisper `abort_callback` (`src/AI/WhisperTranscribe.cpp:129-133`), OpenVINO aux frontières de phase, audio.cpp par `TerminateProcess`, Python par `shouldContinue` sondé toutes les 200 ms + timeout nouveau.

Plusieurs clients : chaque événement est diffusé à **tous** les abonnés ; `history.changed.source` et `job.*.source` portent `api:<client>` (champ `client` du `session.hello` émetteur) ou `menu`, de sorte qu'une session Claude voit les éditions faites par l'autre fenêtre VS Code ou à la souris ; un `job.cancel` n'est accepté que du client qui a lancé le job, sauf `force:true`.

### 6.8 Erreurs typiques

```json
{"id":"30","ok":false,"error":{"code":"busy","message":"A mutating job is running","details":{"job":"j-15","cmd":"ai.noiseSuppress"}}}
{"id":"31","ok":false,"error":{"code":"timeout","message":"Message thread did not respond within 30000 ms","details":{"modalActive":true,"hint":"a modal dialog is probably open"}}}
{"id":"32","ok":false,"error":{"code":"unsupported_platform","message":"audio.cpp runner is Windows-only","details":{"platform":"macos","see":"src/AudioCpp.cpp:713"}}}
{"id":"33","ok":false,"error":{"code":"model_not_installed","message":"whisper-large-v3 is not installed","details":{"missing":["ggml-large-v3.bin"],"sizeBytes":3100000000,"hint":"call ai.models.install"}}}
{"id":"34","ok":false,"error":{"code":"failed","message":"AI processing failed on CPU: …","details":{"manualInstall":"<texte de buildNsFailureDetails>"}}}
```

### 6.9 Forme CLI

`SpectroCraft --json req.ndjson --json-out res.ndjson` (ou `-` pour stdin) : mêmes lignes, sans `session.hello` ; lecture ligne à ligne (streaming, §5.2), réponse vidée après chaque ligne ; jobs attendus (pompe de messages) et inlinés, `timeoutMs` par ligne (défaut 30 s) sinon `timeout` + annulation ; `{"continueOnError":true}` en première ligne ; code de retour 0 (tout ok) / 1 (au moins une erreur) / 2 (usage, JSON illisible, flag inconnu, ligne > 1 Mio). Windows : exe GUI sans console ; fonctionne seulement avec des handles hérités (`stdout=PIPE`, `--json-out <fichier>` recommandé par sûreté).

---

## 7. Intégration VS Code + Claude Code

### 7.1 Choix du transport

Adaptateur MCP en **Python 3.11+** (SDK officiel `mcp`, FastMCP), transport **stdio** entre Claude Code et l'adaptateur, **TCP loopback NDJSON** entre l'adaptateur et l'app. Raisons vérifiées : JUCE n'a ni serveur HTTP ni SSE (`extern/JUCE/modules/juce_core/network/` ne contient que IPAddress, MACAddress, NamedPipe, Socket, URL, WebInputStream) ; un transport stdio dans l'app serait corrompu par les sous-processus qui héritent de stdin (`src/PythonScripts.cpp:752`, `:857-863`, `src/AudioCpp.cpp:564`) ; l'exe GUI Windows n'a pas de console (`src/CMakeLists.txt:2`).

### 7.2 Arborescence `tools/mcp/`

- `pyproject.toml` — `[project.scripts] spectrocraft-mcp = "spectrocraft_mcp.server:main"`, dépendance `mcp>=1.2`.
- `spectrocraft_mcp/discovery.py` — liste `<appData>/SpectroCraft/control/*.json` aux chemins réels, supprime les fichiers dont le PID est mort, vérifie `startedAt` contre la date de création du processus (PID réutilisé sous Windows), choisit `SPECTROCRAFT_PID` sinon la plus récente.
- `spectrocraft_mcp/client.py` — `socket.create_connection`, `makefile("rw")`, `session.hello` avec `subscribe` et `sinceSeq`, thread lecteur qui route par `id` et pousse les `event` dans une file, `call()`, `wait_job(job, on_progress)`, reconnexion silencieuse.
- `sc_client.py` — client **sans dépendance** (socket + json stdlib) : `python3 tools/mcp/sc_client.py app.state`, pour diagnostiquer quand le paquet `mcp` manque.
- `spectrocraft_mcp/server.py` — FastMCP.

### 7.3 Outils exposés

| Outil | Description | Commande sous-jacente |
|---|---|---|
| `sc_status` | instances détectées, build, document actif ; sinon message explicite (« contrôle désactivé dans Window > Allow AI control » / « utilise sc_launch ») | découverte + `session.hello` |
| `sc_launch(files=[])` | **avant** de lancer : sonde le tube `SpectroCraft.FileForward` (`include/SingleInstance.h:15`) en envoyant `int32 0` et en attendant l'ack `0x01` (`src/SingleInstance.cpp:112-137` : zéro chemin, aucun callback, inoffensif) ; si une instance répond mais qu'aucun `<pid>.json` vivant n'existe → erreur immédiate « instance ouverte sans contrôle : Window > Allow AI control » au lieu d'attendre 20 s. Sinon lance `SPECTROCRAFT_EXE --control`, stdin=DEVNULL, attend ≤ 20 s le fichier de découverte ; `--control` saute `handleStartup`, donc le lancement réussit même avec `multi_instance=0` (§5.2 ControlServer) | — |
| `sc_state` | état complet | `app.state` |
| `sc_history(doc=None, include_journal=True)` | historique + journal | `history.list` |
| `sc_undo` / `sc_redo` / `sc_jump(state)` | — | `history.*` |
| `sc_run_step(type, params, start, end, channels)` | n'importe quel des 20 types sur une plage explicite | `step.run` |
| `sc_run(cmd, params, doc, wait=True, timeout_ms)` | passe-plat + attente de job avec `ctx.report_progress` | toute commande |
| `sc_open` / `sc_save` / `sc_close` | `confirm` obligatoire dans la description | `doc.*` |
| `sc_models` / `sc_install_model` / `sc_devices` | — | `ai.models.*`, `ai.devices` |
| `sc_noise_suppress` / `sc_transcribe` | jobs avec progression relayée | `ai.*` |
| `sc_measure_levels` / `sc_third_octave` / `sc_spectrum` / `sc_export_spectrogram` | — | `measure.*`, `export.spectrogram` |
| `sc_jobs` / `sc_cancel` | — | `job.*` |
| `sc_headless(requests)` | subprocess `SPECTROCRAFT_EXE --json - --json-out <tmp>` (stdout=PIPE, close_fds) | mode CLI |
| **Outils dynamiques** `sc.<domaine>.<commande>` | générés au premier contact depuis `app.describe` : le `params` de chaque commande (JSON Schema restreint généré des `ParamSpec`, §5.2 et exemple §6.2) devient l'`inputSchema` de `list_tools` lowlevel sans transformation ; `doc`, `timeoutMs` et `confirm` sont ajoutés par l'adaptateur comme propriétés optionnelles ; toute commande ajoutée en C++ apparaît sans toucher au Python. Test pytest : chaque `inputSchema` publié est validé par `jsonschema.Draft202012Validator.check_schema` | — |
| Ressources `spectrocraft://commands`, `spectrocraft://journal/tail` | describe complet, JSONL du jour | — |

### 7.4 `.mcp.json.example` (gabarit versionné) → `.mcp.json` (local, dans `.gitignore`)

Le chemin de l'exécutable est propre à chaque machine : le dépôt versionne un **gabarit** `.mcp.json.example` ; chacun le copie en `.mcp.json` (ignoré par git) et ajuste `SPECTROCRAFT_EXE`. Contenu du gabarit (valeurs macOS) :

```json
{"mcpServers":{"spectrocraft":{
  "command":"uv","args":["run","--directory","tools/mcp","spectrocraft-mcp"],
  "env":{"SPECTROCRAFT_EXE":"/Applications/SpectroCraft.app/Contents/MacOS/SpectroCraft",
         "SPECTROCRAFT_LAUNCH":"ask"}}}}
```

Sans `uv` : `"command":"python3","args":["-m","spectrocraft_mcp"]` après `pip install -e tools/mcp`. Windows : `"command":"py","args":["-3","-m","spectrocraft_mcp"]`, `SPECTROCRAFT_EXE` vers `build\SpectroCraft_artefacts\Release\SpectroCraft.exe`. Un `CLAUDE.md` du dépôt décrit le vocabulaire (docId, secondes, dB, `confirm`).

### 7.5 Cycle de vie

1. Nicolas ouvre SpectroCraft ; le réglage `control_enabled` (menu Window > « Allow AI control (localhost) ») ou le flag `--control` démarre `ControlServer`, qui écrit `<pid>.json`. Indicateur dans la barre d'état quand un client est connecté.
2. Claude Code lance l'adaptateur (stdio) à l'ouverture du workspace ; connexion paresseuse au premier outil.
3. Plusieurs instances (`multi_instance=1` par défaut, `src/SingleInstance.cpp:32`) : chacune a son port et son fichier ; `sc_status` les liste avec leurs documents ouverts ; `sc_run` accepte `pid`. **Instance ouverte mais contrôle désactivé** : aucun `<pid>.json`, mais le tube `FileForward` répond à la sonde (§7.3) → `sc_status` renvoie « instance ouverte sans contrôle (pid inconnu sur POSIX) : activer Window > Allow AI control, ou relancer avec `--control` » et `sc_launch` ne lance rien sans `force=True`. Avec `multi_instance=0`, un `SpectroCraft --control` lancé par l'adaptateur **ne doit pas** forwarder-puis-quitter (`src/SingleInstance.cpp:191-213`) : `--control` saute `handleStartup` (§5.2) ; la nouvelle instance contrôlable coexiste alors avec l'ancienne, ce que `sc_status` indique.
4. Aucune instance : établi par « aucun `<pid>.json` vivant **et** sonde du tube sans réponse » (sur POSIX il n'existe pas d'autre sonde, `src/SingleInstance.cpp:206-208` ; la sonde coûte au plus le délai de connexion du tube). Selon `SPECTROCRAFT_LAUNCH` — `ask` (erreur explicite proposant `sc_launch`), `auto` (Popen détaché), `never` (bascule sur `sc_headless`).
5. Fermeture de l'app : `shutdown()` arrête le serveur avant `mainWindow.reset()` et supprime le fichier ; l'adaptateur retente la découverte à l'appel suivant. Fermeture de VS Code : l'app continue ; un job mutant en cours va au bout et reste consultable via `job.get` depuis une nouvelle session.
6. Boucle de développement : `SC_TEST_FILTER=control ./SpectroCraftTests --run-unit-tests` puis `python3 tools/mcp/sc_client.py app.state` contre l'app ouverte.

### 7.6 Exemple de dialogue Claude ↔ app

> **Nicolas** : Ouvre recording_b.wav, sélectionne 2–3,5 s, mesure les niveaux, débruite avec DeepFilterNet2 sur CPU et dis-moi ce qui a changé.

```
Claude → sc_status                      ← {instances:[{pid:51233, docs:["recording_a.wav"]}]}
Claude → sc_open(path="…/recording_b.wav")← {doc:8, durationSec:30.0, channels:4, sampleRate:48000}
Claude → sc.selection.set(start=2.0,end=3.5,channels=[0,1],doc=8)
Claude → sc_measure_levels(start=2.0,end=3.5,channels=[0,1],doc=8)
                                        ← ch0 rmsDb:-18.3 peakDb:-6.1 (75.7 dB-SPL) ; ch1 …
Claude → sc_noise_suppress(modelId="deepfilternet2",device="CPU",attenLimDb=12,doc=8,wait=True)
         (progression relayée : compiling 20 % … inferring 85 %)
                                        ← job.done {patchSeq:19, deviceUsed:"CPU", compileMs:8400, inferMs:2210}
Claude → sc_measure_levels(…)           ← ch0 rmsDb:-24.9 peakDb:-7.0 …
Claude → sc_history(doc=8)              ← states:[Loaded, {opId:"ai.noiseSuppress", params:{…}, source:"api:claude-code"}]
```

> **Claude** : Débruitage appliqué sur 2,0–3,5 s (DeepFilterNet2, CPU, 10,6 s). Le RMS du canal 0 est passé de -18,3 à -24,9 dBFS (75,7 → 69,1 dB-SPL), le pic de -6,1 à -7,0 dBFS. Une entrée d'historique, annulable (`sc_undo`). Le History panel de l'app affiche la ligne avec la source `api:claude-code`.

---

## 8. Plan de livraison par lots

| Lot | Contenu | Fichiers principaux | Jours | Valeur pour Nicolas | Dépend de |
|---|---|---|---|---|---|
| **0** — Registre minimal, `step.run`, journal de session, `--json`, correctifs Actions | `CommandRegistry`/`Context`/`Outcome`/`Spec`/`invoke` ; `UiBridge` (interface + impl nulle) ; `step.run`/`steps.run`/`step.validate`, `app.describe`, `app.state`, `doc.open` (sync petit / `loadFileAsync` pompé), `doc.list`, `selection.set/all`, `history.list/jumpTo/undo/redo`, `doc.meta`, `doc.saveAs` ; `FileDoc::id` ; puits de session dans `recordActionStep` (`src/Application.cpp:355`) ; `AlertGate::ScopedCapture` ; correctifs `src/Actions.cpp` (case `'e'`, `resolveAiDevice`, `unsupported_build`) ; mode `--json` avec pompe, codes 0/1/2, refus des flags inconnus ; tests `testControlRegistry` (parcours de **tous** les `actionTypes` avec validate + apply), `testControlDescribe` (schéma généré ↔ `expect()`), `testControlJsonCli` (stdin streaming, codes 0/1/2, timeout d'un `LongRunning`) — tous dans la suite `--run-unit-tests` que ctest lance déjà sur les trois OS du CI | `include/control/CommandRegistry.h`, `src/control/CommandRegistry.cpp`, `include/control/UiBridge.h`, `src/control/StateJson.cpp`, `src/control/commands/Core.cpp`, `src/control/commands/History.cpp`, `include/AlertGate.h`, `include/FileDoc.h`, `src/Actions.cpp`, `src/Application.cpp`, `src/Main.cpp`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/regression/control.inc` | **2,5** | Claude Code pilote SpectroCraft en Bash (`SpectroCraft --json -`) avec un contrat JSON : ouvrir, sélectionner une plage précise, appliquer n'importe laquelle des 20 étapes, lire l'historique, sauver ; trois défauts de rejeu corrigés pour le batch existant. **Limite** : le journal ne voit, jusqu'au lot 5, que les éditions par dialogue (`recordActionStep`) et les commandes API, pas cut/paste, marqueurs ou undo à la souris (§5.2 Journal) | — |
| **1** — Serveur de contrôle dans l'app ouverte + JobManager minimal | `ControlServer` + `ClientSession` (port 0, NDJSON, max 4, un writer), arrêt avec auto-connexion avant `close()` ; `PendingCall` + timeout 30 s + `abandoned` ; garde modale dans le lambda ; jeton, `session.hello`, pause après 3 échecs ; fichier de découverte 0600, balayage ; réglage `control_enabled` + item Window + `--control` + indicateur ; `UiBridge` implémenté par `MainWindow` ; `JobManager` (1 mutant max, commit par hop) pour streamé/open > 200 Mo/`doc.save` (`runSaveAsync` extrait vers `Application::saveDocument`) ; `view.*`, `transport.*` (`ensureAudioReady(2000)`), `doc.close`, `markers.add/list` ; événements par chaînage des hooks ; périmètre de chemins `control_roots` + limites de taille (§5.3) ; `testControlServer` avec `ScModalWatchdog`, ajouté à `scIsSerialTest` (`:26973`) : hello/jeton, 3 échecs → pause, course `--control` + `shutdown()` (client connecté et commande en vol pendant la fermeture → `shutting_down`, thread arrêté < 2 s sur Linux), balayage des `<pid>.json` morts (fichier forgé avec un PID inexistant), limite de ligne, `docSwitched` ; port loopback éphémère, donc compatible avec les runners GitHub (aucune règle réseau à ouvrir) | `include/control/ControlServer.h`, `src/control/ControlServer.cpp`, `src/control/ClientSession.cpp`, `src/control/JobManager.cpp`, `src/control/commands/View.cpp`, `src/control/commands/Transport.cpp`, `src/Main.cpp`, `src/MainWindow.cpp`, `include/MainWindow.h`, `include/Settings.h`, `tools/mcp/sc_client.py` | **3,5** | L'app **ouverte** est pilotable depuis un terminal : état, historique, undo/redo, étape sur une plage précise avec rafraîchissement immédiat de l'écran, sans jamais bloquer sur un dialogue ; opt-in, loopback, jeton | 0 |
| **2** — Adaptateur MCP Python + VS Code | package `tools/mcp` (discovery aux vrais chemins, client avec thread lecteur, `wait_job`, reconnexion + `sinceSeq`), `server.py` FastMCP (outils fixes + dynamiques + ressources), `.mcp.json.example`, `CLAUDE.md`, README « Contrôle par IA », tests pytest contre un faux serveur NDJSON (hello, routage par `id`, reconnexion + rejeu `sinceSeq`, `wait_job`, validation des `inputSchema`) et contre `--json` (exe réel, skip si `SPECTROCRAFT_EXE` absent), `smoke.sh` de bout en bout adaptateur ↔ vraie app ouverte (local seulement, pas en CI) ; job CI `mcp-tests` (`actions/setup-python` 3.11, `pip install -e tools/mcp[test]`, pytest, sans build C++) | `tools/mcp/pyproject.toml`, `tools/mcp/spectrocraft_mcp/{discovery,client,server}.py`, `tools/mcp/tests/test_client.py`, `.mcp.json.example`, `.gitignore`, `CLAUDE.md`, `README.md`, `.github/workflows/build.yml` | **3** | Depuis VS Code, « ouvre recording_b.wav, sélectionne 2–3,5 s, applique un fade-out et montre-moi l'historique » fonctionne en langage naturel, l'app réagissant en direct | 1 |
| **3** — Modèles IA, devices, débruitage, transcription en jobs | `ModelCatalog` unifié ; `ai.models.list/install/remove/verify` (`ensureInstalled` en job) ; assets audio.cpp `installable:false` sans SHA-256 ; `ai.devices` (`OvRuntime::devices` `src/AI/OvRuntime.cpp:148` + `resolveAiDevice` `:24`), `ai.setDevice` ; `ai.noiseSuppress/transcribe/batchTranscribe` en jobs après alignement des commits worker-side (`:3951`, `:3562-3565`) ; résultats structurés ; `transcription.get` ; `buildNsFailureDetails` déplacé hors de `MainWindow.cpp:6058` ; `SC_MODELS_DIR` sur Windows ; outils MCP ; `testAiCommands` avec skips gracieux | `include/AI/ModelCatalog.h`, `src/AI/ModelCatalog.cpp`, `src/AI/ModelManager.cpp`, `src/control/commands/Ai.cpp`, `src/Application.cpp`, `src/NoiseSuppression.cpp`, `src/MainWindow.cpp`, `tools/mcp/spectrocraft_mcp/server.py`, `tests/regression/ai_commands.inc` | **3** | « Installe whisper-base et transcris le canal 0 en français », « débruite avec DeepFilterNet3 sur CPU » : Claude lance, suit, annule, reçoit les segments en JSON ; les devices annoncés sont ceux réellement disponibles | 1 |
| **4** — Mesures chiffrées exportables | module `Measures` extrait de `FFTAnalysisView` (static, `windowType` en argument, helpers sortis du file-scope) ; `levels()`, `peaks()`, `harmonics()` ; `measure.levels/spectrum/thirdOctave/curve/probe/spectrogramData/harmonics` ; `export.spectrogram` (`renderToPng` `include/SpectralEngine.h:129` + overlays via `UiBridge`) ; `markers.exportCsv/importCsv` ; tests d'égalité numérique Measures vs caches de la vue et vs tests dB-SPL (`:2688-2775`) | `include/Measures.h`, `src/Measures.cpp`, `src/control/commands/Measure.cpp`, `src/control/commands/Export.cpp`, `src/Panels.cpp`, `include/Panels.h`, `tests/regression/measures.inc` | **3** | Pour les rapports d'essais et de qualification : niveaux RMS/peak par canal et bandes 1/3 d'octave dB-SPL identiques à l'écran, avec unités et pleine échelle, exploitables dans un rapport sans TSV | 1 |
| **5** — Journal persistant, Patch enrichi, événements avec rejeu | anneau 10 000 + JSONL par session/PID, `seq`, `kind`, rotation ; hooks `MarkerManager::onChange`, setters `metaDirty`, vidages avec raison ; `Patch` enrichi (`seq, timestampMs, opId, params, source`) via contexte thread-local dans `recordReplace` + `lastRecorded()` ; `UndoManager::nextSeq` + `onChanged` ; `history.list` complet, `journal.read`, `events.subscribe` avec rejeu ; History panel avec horodatage et source ; `testJournal`, `testHistoryJson` (mémoire d'historique `:21143` inchangée) | `include/control/Journal.h`, `src/control/Journal.cpp`, `include/UndoPatch.h`, `include/UndoManager.h`, `src/AudioEngine.cpp`, `src/Application.cpp`, `src/FileDoc.cpp`, `src/MarkerManager.cpp`, `src/Panels.cpp`, `tests/regression/journal.inc` | **2,5** | « Qu'est-ce qui a été fait sur ce fichier depuis ce matin, avec quels paramètres ? » obtient une réponse exacte et horodatée, marqueurs et vidages d'undo compris ; l'adaptateur ne perd aucun événement après reconnexion | 1 |
| **6** — Éclatement des commandes et migration des menus | `edit.*`/`effect.*`/`gen.*`/`ai.*`/`python.run` avec alias = ancien type ; `applyActionStep`/`validateStepParams`/`isKnownActionType` en façades ; `edit.cut/copy/paste*`, `selection.*`, `tables.*`, `meta.*`, `settings.get/set` (clamps factorisés), `recent.*`, `doc.new/activate` ; migration de `menuItemSelected` (`src/MainWindow.cpp:2773-3139`) et des 27 sites `recordActionStep` (dialogues conservés comme formulaires, `runDestructiveEdit` = enveloppe UI du registre) ; `continueReplay` avec callback de fin au lieu de `showMessageBox` (`:2076`, `:2089`) ; test « menu == commande » via `getLastEffectId` (`include/MainWindow.h:264`) ; `docs/commands.md` généré | `src/control/commands/{Edit,Markers,Tables,Meta,Settings}.cpp`, `src/Actions.cpp`, `include/Actions.h`, `src/MainWindow.cpp`, `src/Application.cpp`, `include/Settings.h`, `docs/commands.md` | **3** | Toute fonctionnalité de menu utile a son équivalent JSON typé et découvrable ; souris et API passent par le même code (même rafraîchissement, même enregistrement, même journal) | 5 |
| **7** — Durcissement, pont Python, flags historiques, stems/SR (Windows) | `runProcess` : timeout, stdin → NUL / `/dev/null`, `PYTHONUTF8=1`, hash SHA-256 du script (`src/AI/Sha256.cpp`) dans le journal, liste blanche `scriptFolder()` + confirm ; `result.json` optionnel relu (marqueurs, texte, mesures), `out.wav` facultatif ; `runProcessWithPipes` stdin fermé ; flags `--test-*` réimplémentés sur `--json` (codes de retour, `--test-batch-transcribe` sur magasin isolé) ; `StemService`/`SuperResService` extraits (`src/StemSeparation.cpp:1055-1280`, `src/SuperResolution.cpp:579-673`) → `ai.stems.run`/`ai.superRes.run` (`WindowsOnly`) ; SHA-256 des assets audio.cpp ; **revue sécurité** sur liste fermée : périmètre `control_roots` et liens symboliques, jeton (génération, non-journalisation, ACL Windows), limites de taille et file d'événements, contenu du journal (aucun texte de transcription), exécution de scripts (`scriptFolder` + hash) et de binaires téléchargés (SHA-256 obligatoire avant `installable:true`), comportement sous `multi_instance=0`, fuzz de 10 000 lignes NDJSON aléatoires sans crash ; **test de charge** : 1000 `app.state` séquentiels sur le poste de dev < 10 s, p99 < 50 ms, mémoire stable (± 5 Mo) après 10 000 commandes | `src/PythonScripts.cpp`, `src/AudioCpp.cpp`, `src/Application.cpp`, `src/Main.cpp`, `src/AI/StemService.cpp`, `src/StemSeparation.cpp`, `src/SuperResolution.cpp`, `src/control/commands/Ai.cpp` | **2,5** | Un script Python lancé par Claude ne peut plus geler l'app ni corrompre un canal et peut renvoyer des marqueurs ou des mesures ; sur Windows, stems et super-résolution pilotables ; protocole documenté pour d'autres clients (outils de test, scripts) | 3 |
| **Total** | | | **23 j** nominaux ; **fourchette 23-30 j** (lot 1 : 3,5-5 ; lot 5 : 2,5-3,5 ; lot 6 : 3-5, le plus incertain ; les autres ± 0,5) | Contrôle de l'app ouverte depuis VS Code à la fin du lot 2 (**9 j**, 9-11 avec marge) ; IA et mesures (lots 3-4) indépendants l'un de l'autre | |

### 8.1 Stratégie de test et hypothèses d'estimation

**CI existant** (`.github/workflows/build.yml`) : trois jobs `windows-latest` (Release, `ctest -j 1` pour sérialiser les téléchargements de modèles), `macos-15` et `ubuntu-24.04` (Debug, shards parallèles, Linux sous `xvfb-run`), tous avec `-DSPECTRO_ENABLE_AI=ON`, modèles mis en cache, tests = la suite embarquée `--run-unit-tests` que ctest lance (tests/CMakeLists.txt : `SC_TEST_SHARDS`). Aucun Python n'est installé par le CI aujourd'hui.

| Niveau | Quoi | Où il tourne |
|---|---|---|
| Unitaire C++ (lots 0, 1, 3-7) | `testControlRegistry`, `testControlDescribe`, `testControlJsonCli`, `testControlServer`, `testAiCommands`, `testMeasures`, `testJournal`, `testHistoryJson` ; `testControlServer` ouvre un port loopback éphémère (`createListener(0,"127.0.0.1")`), permis sur les trois runners ; marqué `scIsSerialTest` (`tests/UnitTests.cpp:26973`) pour ne pas croiser les shards ; gardé par `ScModalWatchdog` (`:27082`) comme `testSingleInstance` (`:25849-25922`) | les 3 jobs existants, sans changement de workflow |
| Comportements spécifiques OS | Windows : `--json` avec handles hérités (exe GUI), ACL du fichier de découverte ; Linux : arrêt du listener (`portNumber=0`, `juce_Socket.cpp:613`) ; macOS : `~/Library/SpectroCraft/control` | chaque job via `#if` dans les tests, assertion plutôt que skip |
| Python (lot 2) | pytest contre un faux serveur NDJSON (pas d'exe) : hello, routage, reconnexion `sinceSeq`, `wait_job`, `inputSchema` valides ; contre `--json` quand `SPECTROCRAFT_EXE` est fourni | nouveau job `mcp-tests` (`actions/setup-python` 3.11 + `pip install -e tools/mcp[test]`), ~1 min, sans build C++ |
| Bout en bout | `smoke.sh` : lance l'app avec `--control`, `sc_client.py app.state`, un `step.run`, un `history.undo`, ferme ; nécessite une session graphique | poste de dev seulement (checklist avant transmission à Max), pas en CI |
| Charge / robustesse (lot 7) | 1000 `app.state` < 10 s, p99 < 50 ms ; 10 000 commandes sans dérive mémoire ; fuzz NDJSON | poste de dev, résultats consignés dans `docs/commands.md` |

**Hypothèses de l'estimation** : jours-personne d'un développeur connaissant JUCE et le dépôt (Nicolas, ou Max pour les lots 6-7), à temps plein équivalent ; chaque lot inclut ses tests (~30 % du lot) et sa documentation (~10 %) ; aucun build CI à réparer en chemin ; audio.cpp non testé hors Windows (un poste Windows est requis pour le lot 7). Incertitude : ± 0,5 j sur les lots 0, 2, 3, 4, 7 ; lot 1 3,5-5 j (threading du serveur, course à l'arrêt) ; lot 5 2,5-3,5 j (hooks dispersés) ; lot 6 3-5 j (migration de 90 cases et 27 sites, régressions UI possibles). Total nominal 23 j, fourchette 23-30 j.

---

## 9. Questions à trancher par Nicolas / Max

1. **Ordre des lots 3 et 4** : le plan livre l'IA (lot 3) avant les mesures (lot 4). Si les mesures comptent davantage pour les utilisateurs, faut-il inverser ? Les deux ne dépendent que du lot 1.
2. **Opt-in** : « Allow AI control (localhost) » désactivé par défaut (recommandé) ou activé sur le poste de dev ? Faut-il en plus une confirmation visuelle non modale dans l'app (toast 30-60 s) pour les opérations destructives lancées par Claude ?
3. **Emplacement du fichier de découverte** : `<userApplicationDataDirectory>/SpectroCraft/control` (macOS `~/Library/SpectroCraft/control`, cohérent avec `scriptFolder` et `models`) — OK, ou à côté de `SpectroCraft.recent` (`~/Library/Preferences` sur macOS) ?
4. **Adaptateur MCP** : Python avec `uv` (cohérent avec le pont Python) ou `pip install -e` ? TypeScript si zéro dépendance Python est requis sur les postes verrouillés ?
5. **Outils MCP** : jeu typé nombreux (`sc_noise_suppress`, `sc_measure_levels`…) ou jeu minimal (`sc_run` + `sc_commands`) complété par les outils dynamiques ? Les deux sont prévus ; la question est ce qui doit apparaître dans la liste de Claude Code.
6. **Journal persistant** (JSONL dans `~/Library/SpectroCraft/journal`, contient des chemins de fichiers) : souhaité dès le lot 5, ou mémoire seulement ?
7. **Première édition après un retour en arrière** : accepter silencieusement (pile redo vidée, signalée par `redoDiscarded` — recommandé) ou exiger `confirm:true` pour les commandes venant de l'API ?
8. **Mode `--json` sur fichier streamé (> 200 Mo)** : matérialiser sans limite ou refuser au-delà d'un seuil de RAM ? `--json` doit-il remplacer à terme les flags `--test-*`/`--run-action` (dépréciation) ?
9. **Migration des menus (lot 6, 3 j)** : la faire, ou accepter que menus et API restent deux chemins et que les éditions à la souris n'aient pas de type/params avant le lot 5 ? C'est le lot le plus risqué mais c'est lui qui garantit « même code partout ».
10. **Stems / super-résolution (lot 7, Windows seulement)** : prioritaire, ou à décider avec Max selon la migration vers audio.cpp ? Ajouter les SHA-256 des assets audio.cpp au catalogue (préalable à toute installation par l'API) ?
11. **Version du protocole** : figer `protocol:1` dès le lot 1 avec engagement de compatibilité pour d'autres clients, ou le laisser expérimental jusqu'au lot 6 ?
12. **Pour Max** : les trois correctifs de `src/Actions.cpp` (case `'e'`, `resolveAiDevice`, erreur en build non-AI) et l'alignement des commits worker-side (`src/Application.cpp:3951`, `:3562-3565`) sont des bugs indépendants du contrôle IA — les livrer comme PR séparée avant le lot 0 ?
13. **Fork ou contribution amont** : un dossier `src/control/`, un paquet Python `tools/mcp`, un job CI de plus et un gabarit `.mcp.json.example` engagent le mainteneur (SuperMax3d). Max accepte-t-il ces ajouts dans son dépôt (avec lui comme relecteur des lots 0-1), ou Nicolas travaille-t-il sur un fork synchronisé ? Dans le second cas, les lots 6-7 (migration des menus) deviennent très coûteux à rebaser.
14. **Option CMake `SC_ENABLE_CONTROL`** (défaut ON) pour livrer des builds sans serveur de contrôle ni flag `--json` (postes verrouillés) ? Si oui, `applyActionStep` et le registre restent compilés (le rejeu en dépend) ; seuls `ControlServer`, `--control` et l'item de menu disparaissent.
15. **`--control` et `multi_instance=0`** : accepter qu'un `SpectroCraft --control` lancé par l'adaptateur crée une seconde instance à côté de la première (recommandé, §7.5), ou préférer qu'il échoue avec un message ?
16. **Langue du protocole** : identifiants de commandes et `message` d'erreur en anglais (recommandé : cohérent avec le code, les alertes et Claude Code), documentation `docs/commands.md` en français ?
17. **Versions épinglées sur les postes verrouillés** : Python 3.11+ et SDK `mcp>=1.2` sont-ils installables (proxy, politique logicielle) ? Sinon, `sc_client.py` sans dépendance suffit pour un premier usage, et un adaptateur TypeScript reste possible (question 4).
18. **Périmètre de chemins** (§5.3) : racines par défaut = dossier personnel + `scriptFolder()` + temporaire ; faut-il y ajouter d'emblée les partages réseau de l'équipe ou exiger `confirm` à chaque accès hors du dossier personnel ?

---

## 10. Annexe : méthode de l'audit et chiffres

**Méthode.** Lecture seule du dépôt, aucun build, aucune exécution.

1. **Huit lecteurs** spécialisés, un par sous-système : CLI/IPC (`Main.cpp`, `SingleInstance.cpp`), Actions, historique (`UndoManager`/`UndoPatch`/`HistoryPanel`), modèles IA (`ModelManager`, `NoiseSuppression`, `Whisper*`, `AudioCpp`, `StemSeparation`, `SuperResolution`), API `Application`, mesures et données d'analyse (`FFTAnalysisView`, `SpectralEngine`, `FileMeta`, `Tables`, `MarkerManager`), pont Python et sous-processus, persistance/réglages/harnais de tests. Chaque lecteur a produit une carte (points d'entrée, inventaire, manques, réutilisables, risques) et quatre affirmations clés.
2. **Vérifications adversariales** : les 32 affirmations ont été relues dans le code par un vérificateur distinct ; 15 confirmées, 17 partielles (aucune réfutée en bloc). Les corrections sont intégrées au §3.
3. **Trois architectes** ont proposé chacun une conception complète (pont minimal ; serveur embarqué + registre unique ; bus de commandes complet) avec protocole, intégration VS Code et plan de lots.
4. **Trois juges** indépendants ont noté chaque conception (couverture, effort, risque, maintenabilité, utilisabilité IA) avec une vérification de faisabilité dans le code pour chaque affirmation technique ; ils ont relevé sept erreurs factuelles (arrêt du listener JUCE, chemin macOS, `toBundle` inexistant, adressage par index, `setHeadless(false)`, `wait(-1)` sous modal loops, pompe absente en `--json`) — toutes vérifiées et corrigées dans la conception finale.
5. **Synthèse** : conception B retenue à l'unanimité, réordonnée (contrôle de l'app ouverte d'abord) et greffée des meilleures idées de A et C (§5, §8).
6. **Critique finale** par le rédacteur : spot-checks sur `src/Actions.cpp` (cases du switch, ligne 441), `juce_Socket.cpp:155-160`, comptages `recordActionStep` (27) et `runModalLoop` (32 + 28), `JUCE_MODAL_LOOPS_PERMITTED`, `kMaxLevels`, ordre `forwarder`/`mainWindow` dans `shutdown`.
7. **Révision après critique externe** (~150 références relues par un relecteur distinct) : six affirmations corrigées (`onStateChange` : 14 sites et non un seul ; 11 flags + 5 sous-options et non 17 ; version `1.0.72` ; `:1187`/`:4170` couverts par `AlertGate` ; tokenisation toutes plateformes ; preuve du défaut `multi_instance`), et dix manques comblés (`--control` × `multi_instance=0`, document non actif, périmètre de chemins et limites, format de `paramsSchema`, annexe §11, exemples JSON manquants, portée du journal au lot 0, stratégie de test liée au CI, marge d'estimation, questions 13-18).

**Chiffres.**

| Mesure | Valeur |
|---|---|
| Lignes dans `src/*.cpp` | 46 762 |
| Lignes dans `tests/UnitTests.cpp` | 27 429 (+ 9 `tests/regression/*.inc`) |
| Occurrences de `Application app` dans les tests | 268 |
| Flags CLI reconnus | 11 principaux + 5 sous-options ; 0 en JSON ; 3 avec code de retour fiable |
| Types d'étapes d'action | 20 ; 19 exécutables headless |
| MenuIDs | 91 déclarés, 90 traités, 1 mort |
| Sites `recordActionStep` dans `MainWindow.cpp` | 27 |
| `runModalLoop` bruts | 60 (32 `MainWindow.cpp` + 28 `Panels.cpp`) |
| Modèles IA avec SHA-256 | 15 ; assets audio.cpp sans checksum : 4 binaires + 11 modèles |
| Clés du `PropertiesFile` | 79 (69 nommées + `recent_1..10`) |
| Profondeur undo | 64 |
| Affirmations vérifiées | 32 (15 confirmées, 17 partielles) |
| Conceptions / juges | 3 / 3 |
| Lots / jours | 8 / 23 nominaux (fourchette 23-30) |
| MenuIDs par lot (annexe §11) | lot 0 : 24 ; lot 1 : 23 ; lot 4 : 5 ; lot 6 : 36 ; lot 7 : 2 ; mort : 1 |

---

## 11. Annexe : couverture des 91 MenuIDs (exigence 2)

Source : enum `MenuIDs` (`include/MainWindow.h:485-547`, 93 énumérateurs dont les sentinelles `RecNoDeviceItem` et `RecDeviceEnd` exclues) et `menuItemSelected` (`src/MainWindow.cpp:2773-3139`). « Chemin actuel » = ce que le menu appelle aujourd'hui ; « Lot » = premier lot où la commande JSON existe. Les 18 MenuIDs couverts par une étape d'action sont ceux marqués `step.run`.

| Menu | MenuID(s) | Chemin actuel (`src/MainWindow.cpp`) | Commande JSON | Lot |
|---|---|---|---|---|
| File | `FileOpen` | `handleFileOpen()` `:2959` | `doc.open` | 0 |
| File | `FileSave`, `FileSaveAs` | `handleFileSave/SaveAs()` `:2971-2972` → `runSaveAsync` `:2318` | `doc.save` / `doc.saveAs` (sync petit fichier au lot 0 ; job + `Application::saveDocument` extrait au lot 1) | 0 / 1 |
| File | `FileClose`, `FileCloseAll` | `handleCloseFile()` `:2968`, `handleCloseAll()` `:2961` | `doc.close {confirm}`, `doc.closeAll` | 1 |
| File | `FileQuit` | `closeButtonPressed()` `:3004` (refuse si job) | `app.quit {confirm}` | 1 |
| File | `FileNew` | `showNewFileDialog()` `:2958` | `doc.new {channels, sampleRate, durationSec}` | 6 |
| File | `MetaEmbedded`, `MetaExternal`, `MetaSaveOnly` | `setActiveDocMetaMode` `:2973-2978` (`include/Application.h:412`), `handleSaveMetadataOnly()` `:2965` | `meta.setMode`, `meta.save` | 6 |
| File | `FileExportSpectral` | `handleExportSpectral()` `:2979` → `preparePngExport` `:2536` | `export.spectrogram` (overlays via `UiBridge`) | 4 |
| File | `ExpAxes`, `ExpMarkers`, `ExpSelections`, `ExpDrawings` | bascules `app.settings.export*` `:2988-3003` | paramètre `overlays` de `export.spectrogram` + `settings.set` | 4 |
| File | `FileSnapshot` | `takeSpectrogramSnapshot()` `:2983` (snapshots non persistés, `include/Panels.h:678`) | `export.snapshot` (`NeedsUi`) | 6 |
| File | `RecentBase+i`, `ClearRecent` | `app.recentFiles` `:2807-2822` | `recent.list/open/remove/clear` | 6 |
| Edit | `EditUndo`, `EditRedo` | `undoStep/redoStep` + `refreshAfterEdit` `:3006-3007` | `history.undo/redo` | 0 |
| Edit | `EditDelete`, `EditTrimSelection` | `recordActionStep` + `runDestructiveEdit` `:3024-3029` | `step.run {type:"delete"|"trim"}` | 0 |
| Edit | `EditSelectAll` | `app.selectAll()` `:3030` | `selection.all` | 0 |
| Edit | `EditCut`, `EditCopy`, `EditPaste`, `EditPasteMix`, `EditPasteToNewChannels`, `EditCopyToNewFile` | `app.cut/copy/paste/pasteMix/pasteToNewChannels/copyToNewFile` `:3008-3023` (aucun type d'étape, presse-papiers interne) | `edit.cut/copy/paste/pasteMix/pasteToNewChannels/copyToNewFile` | 6 |
| Edit | `EditSnapToZeroCrossings` | `app.settings.snapToZeroCrossings` `:3031-3034` | `settings.set` | 6 |
| View | `ViewZoomIn/Out/Fit/ToSelection/OutFull` | `zoomHorizontal/zoomToFit/zoomToSelection/zoomOutFull` `:3036-3040` | `view.zoom {factor|to}` (`viewStart/viewEnd`, `include/Application.h:120`, `:483`) | 1 |
| View | `ViewVerticalZoomIn/Out` | `zoomVerticalBoth` `:3041-3042` (membre `MainWindow`) | `view.zoom {vertical}` (`NeedsUi`) | 1 |
| View | `ViewShowWaveform`, `ViewShowSpectral`, `ViewShowLeftPanels` | membres `MainWindow` `:2887-2906` | `view.show {waveform, spectral, leftPanels}` (`NeedsUi`) | 1 |
| View | `ViewShowAllChannels`, `ChannelBase+i` | `:2849`, `toggleChannelVisible`/`visibleChannels` `:2829-2844` (`include/Application.h:260`) | `view.channels {visible}` | 1 |
| View | `ViewAxisOnRight`, `ViewRulerAtBottom`, `ViewShowAmpGrid`, `ViewShowFreqGrid`, `ViewShowThirdOctGrid`, `ViewShowDedTrackGrid` | bascules `app.settings.*` + `onStateChange` `:2911-2952` | `settings.set` | 6 |
| View | `ViewColourMap` | aucun case (`include/MainWindow.h:503`) | — (mort) | — |
| Selection | `SelectionToolTimeRange/Marquee/Lasso/Paintbrush`, `SelectionClear`, `SelectionBrushSmaller/Larger` | `setSelectionTool`, `clearSpectralSelection`, `stepBrushRadius` `:2858-2877` | `selection.tool`, `selection.clearSpectral`, `selection.brush` | 6 |
| Transport | `TransportPlay/Stop/Pause/Loop` | `playWithAudioInit`, `app.stop/pause/toggleLoop` `:3044-3047` | `transport.play/stop/pause/loop` (`ensureAudioReady(2000)`) | 1 |
| Transport | `TransportPlaySpectralSel` | `app.playSpectralSel` + `pushSpectralMaskToEngine` `:3048-3056` | `transport.playSpectralSel` | 1 |
| Transport | `TransportRecord` | `handleStartStopRecording()` `:3058` → `startRecording/stopRecording` (`src/Application.cpp:1129`, `:1194`, notifient déjà `onStateChange`) | `record.start/stop` | 6 |
| Transport | `RecOptUnlimited/Loop/Timed`, `RecDeviceBase+n` | `setRecordingMode` `:3065-3090` (dialogues de durée), sous-menu devices | `record.setMode {mode, seconds}`, `record.setDevice` | 6 |
| Effects | `ToolsSilence`, `ToolsFadeIn`, `ToolsFadeOut`, `ToolsNormalize`, `ToolsReverse`, `ToolsInvert` | `recordActionStep` + `runDestructiveEdit` `:3096-3103`, `:3126-3129` | `step.run` | 0 |
| Effects | `ToolsChangeGain`, `ToolsTimeStretch`, `ToolsCatTimeScale`, `ToolsFilterEq`, `ToolsEnvelope`, `ToolsResample` | dialogues `show*Dialog()` `:3104-3105`, `:3130-3133` → `recordActionStep` (`:3808`, `:4735`, `:4991`, `:5236`, `:5481`, `:5674`) | `step.run` (le dialogue devient un formulaire ; `envelope` après le correctif `case 'e'`) | 0 |
| Effects | `ToolsGenSilence`, `ToolsGenNoise`, `ToolsGenTone` | dialogues `:3114-3116` → `recordActionStep` (`:4350`, `:4505`, `:4626`) | `step.run {type:"gen*"}` | 0 |
| Effects | `ToolsNoiseSuppress` | `showNoiseSuppressDialog()` `:3118` → `runNoiseSuppressAsync` `:6255` | `step.run {type:"noiseSuppress"}` (sync, lot 0) ; `ai.noiseSuppress` en job (lot 3) | 0 / 3 |
| Effects | `ToolsSplitSpectralSelCfg`, `ToolsSplitSpectralSelExec` | `showSplitConfigDialog()`, `runSplitExecute()` `:3108-3113` | `edit.splitSpectralSel {mapping}` | 6 |
| Effects | `ToolsStemSeparate`, `ToolsSuperResolve` | `showStemSeparationWindow/showSuperResolutionWindow` `:3106-3107` | `ai.stems.run`, `ai.superRes.run` (`WindowsOnly`) | 7 |
| Actions | `ActionsRunSelected` | `runAction(index)` `:3120` | `steps.run` (lot 0) ; `actions.list/run {name}` (lot 6) | 0 / 6 |
| Actions | `ActionsBatch` | `showActionBatchWindow` `:3123` → `sc::runBatch` (`src/Actions.cpp:649`) | `actions.batch {name, inDir, outDir}` | 6 |
| Window | `WindowPanelBase+i` | `setPanelVisible/isPanelOpen` `:2785-2790` | `ui.panel.set {id, visible}` (`NeedsUi`, via `UiBridge`) | 1 |
| Window | `WindowAllowMultiple` | `app.settings.allowMultipleInstances` + `saveSettings` `:2796-2799` | `settings.set {multi_instance}` | 6 |
| Help | `HelpAbout` | `showAboutDialog` `:3135` | `session.hello` (version, build) ; pas de dialogue | 1 |

**Totaux** : lot 0 : 24 ; lot 1 : 23 ; lot 4 : 5 ; lot 6 : 36 ; lot 7 : 2 ; mort : 1 — soit 91. À la fin du lot 1, 47 MenuIDs (52 %) ont un équivalent JSON ; à la fin du lot 6, 88 (97 %), les deux restants étant Windows-only (lot 7).

**Hors MenuIDs** (actions de panneaux et de souris, à couvrir pour que « activer les fonctionnalités » soit complet) : marqueurs CSV (`MarkerManager::saveCsv/loadCsv`, `src/MarkerManager.cpp:745`, `:757` → `markers.exportCsv/importCsv`, lot 4) ; tables (`serialiseTables`, `src/Tables.cpp:665` → `tables.*`, lot 6) ; dessins, clone pick et gestes de sélection (`startClonePick/endClonePick/clonePickClick`, `startSelGesture/selGestureClick`, `src/Application.cpp:1669-1799` → `drawing.*`, `clone.*`, lot 6 ; les gestes eux-mêmes restent souris) ; transcription depuis le panneau (`:6516` → `ai.transcribe`, lot 3) ; script Python depuis le panneau (`:260` → `python.run`, lot 6) ; sélections nommées (`SavedSelections` → `selection.saved.*`, lot 6).

Fichiers de l'audit : `/Volumes/Extreme SSD/github_external/SuperMax3d/SpectroCraft-audit/phase9-ai-control/RAPPORT.md` (ce document).
