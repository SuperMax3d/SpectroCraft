# Lot 7 (partie « cœur ») — durcissement du pont Python, `result.json`, services stems / SR

Branche `ai-control-hardening` (fork `nicolasmarcotteTaintin/SpectroCraft`), créée depuis `ai-control`
= `76de144`. Travail fait en parallèle du lot 0+ : rien ici ne dépend du `CommandRegistry` ; le
lot 7 n'aura plus qu'à écrire des enveloppes minces (voir « À faire à la fusion »).

## Fait

### 1. Pont Python durci (`src/PythonScripts.cpp`, `include/PythonScripts.h`)

- **`runProcessEx(cmd, args, RunOptions, shouldContinue) -> ProcessResult`** (`started`, `cancelled`,
  `timedOut`, `exitCode`, `stdoutText`, `stderrText`, `elapsedMs`). `runProcess()` devient une
  enveloppe avec l'ancien contrat (pas de timeout).
  - **Timeout** `RunOptions::timeoutMs` (0 = aucun). Timeout et annulation tuent **l'arbre** :
    - Windows : *job object*, processus créé `CREATE_SUSPENDED`, assigné au job puis relancé ;
      `TerminateJobObject`, puis attente bornée que le job soit vide (fichiers libérés) ;
      `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (couvre aussi un crash de SpectroCraft) ;
    - POSIX : groupe de processus propre (`setpgid` côté enfant et parent), `SIGTERM` au groupe,
      délai `killGraceMs` (1 s), puis `SIGKILL` au groupe ; `PR_SET_PDEATHSIG` en plus sur Linux.
  - `killTreeOnExit` (vrai par défaut) : les petits-enfants encore vivants à la sortie normale du
    script sont aussi tués (voir Écarts).
  - **stdin** = `NUL` (Windows) / `/dev/null` (POSIX) : un script qui lit stdin reçoit EOF.
  - **Environnement** : `PYTHONUTF8=1`, `PYTHONIOENCODING=utf-8` (bloc UTF-16 trié +
    `CREATE_UNICODE_ENVIRONMENT` sous Windows ; `envp` + `execve` sous POSIX, recherche dans `PATH`
    faite avant `fork`, aucun appel non *async-signal-safe* dans l'enfant).
  - **Décodage** `decodeProcessOutput` : UTF-8 si valide, sinon page ANSI (Windows) / Latin-1 ; un
    octet NUL devient U+FFFD au lieu de tronquer.
  - **Chemins** avec espaces / non-ASCII : ligne de commande UTF-16 avec la citation exacte de
    `CommandLineToArgvW` (Windows), argv UTF-8 (POSIX). Testé sur macOS (voir Tests).
  - Échec d'`exec` détecté sous POSIX aussi (tube *close-on-exec*) → `started=false` (avant : vrai
    avec code 127).
  - Windows : `CREATE_NO_WINDOW` (plus de console qui clignote à chaque script).
- **`scriptSha256(file)`** : SHA-256 des **octets sur disque** (ce qui s'exécute ; `replaceWithText`
  écrit des CRLF, donc ce n'est pas le hash du texte de l'éditeur). `src/AI/Sha256.cpp` est
  maintenant compilé dans toutes les configurations (il ne l'était qu'avec l'IA).
- **`isUnderScriptFolder(script, &reason, folder = scriptFolder(), &resolved)`** : liens
  symboliques résolus (`realpath` / `GetFinalPathNameByHandleW`), extension vérifiée sur la cible ;
  raisons stables `not_found`, `not_python`, `outside_script_folder`. `resolved` = chemin à
  exécuter et à hacher (le même que celui vérifié).
- **`runScript(pythonCmd, script, exchange, options, shouldContinue) -> ScriptRunReport`** : un run
  complet (processus + `out.wav` + `result.json`) sans toucher de document.
- **`ScopedScriptRunCapture`** (thread-local, imbricable) : `Application::runPythonScript` garde sa
  signature ; un appelant qui ouvre une capture sur son thread impose ses `RunOptions` (timeout) et
  reçoit le `ScriptRunReport` (hash, interpréteur, code, sorties, `result.json`, `outWavPresent`,
  `audioChanged`). Sans capture (GUI, actions enregistrées) : comportement inchangé, pas de limite.
- `src/AudioCpp.cpp` (`runProcessWithPipes` uniquement) : stdin = `NUL`, extrémités de lecture non
  héritées. Rien d'autre touché dans ce fichier.

### 2. `result.json` (contrat version 1)

Le script reçoit toujours `python <script> <in.wav> <out.wav> <meta.json>`. `meta.json` contient en
plus `inputWav`, `outputWav`, `resultJson` (chemins) et `resultContract` (le schéma). Le script
**peut** écrire un objet JSON UTF-8 à `meta["resultJson"]` (même dossier que `out.wav`) :

```json
{
  "version": 1,
  "timeBase": "input",
  "markers": [ {"time": 1.25, "end": 2.0, "label": "climb"}, {"time": 3.5, "label": "click"} ],
  "text": "Peak -3.1 dBFS at 1.25 s",
  "measures": { "rms_db": -18.2, "peak_db": -3.1 },
  "tables": [ {"name": "bands", "columns": ["Hz", "dB"], "rows": [[1000, -20.5], [2000, -22.0]]} ]
}
```

- Tout est facultatif ; champs inconnus et `null` ignorés (conservés dans `ScriptResult::raw`).
- `timeBase` : `"input"` (défaut, secondes depuis le début de `in.wav`) ou `"document"`. Le pont
  calcule `docTime` / `docEnd` (secondes document) pour chaque marqueur.
- `markers[]` : `time` ≥ 0 fini obligatoire ; `end` facultatif (≥ `time` ; absent, `null` ou
  négatif = marqueur ponctuel) ; `label` chaîne facultative ; ≤ 100 000 marqueurs.
- `measures` : objet libre. `tables[]` : `columns` non vide (chaînes), chaque ligne de `rows` a
  autant de valeurs que de colonnes.
- Fichier ≤ 16 Mio. NaN/Infinity ne sont pas du JSON (Python les écrit par défaut) : le contrat dit
  d'écrire `null`.
- **`out.wav` devient facultatif** quand `result.json` existe (script d'analyse). Un `result.json`
  invalide fait échouer le run, document intact. Ni l'un ni l'autre → échec (« wrote neither… »).
- Les scripts existants (exemples, scripts utilisateur) fonctionnent sans changement.
- Contrat documenté dans `resultContract()` (embarqué dans `meta.json`), `resultContractText()`
  (texte d'aide) et le gabarit du bouton Add (`newScriptTemplate`). Les exemples ne sont pas modifiés.

### 3. Services headless stems / super-résolution (`src/AI/StemService.*`, `src/AI/SuperResService.*`)

- `sc::StemService::validate(request, &code)` / `run(request, progress, cancel) -> Result`
  (`ok`, `cancelled`, `errorCode`, `error`, `detail`, `stems[]` {nom, fichier, rate, canaux,
  frames}, `merged` = `sc::MergeResult` au rate source). Requête : mode (SamAudio /
  HtDemucs6 / HtDemucsFt), descriptions + ancres (temps document), binaire (défaut
  `acpp::binaryPath()`), modèle, backend, `extractFromOriginal`, `extStartSecs`, `sourceRate`,
  `input` (canaux), `workDir` (vide = temporaire supprimé ; sinon conservé et `stems[].file` rempli).
- `sc::SuperResService` : idem (FlashSR / AudioSR, `resampleToSourceRate`, `outputFile`, `toolRate`).
- Codes de validation : `invalid_mode`, `no_description`, `invalid_backend`, `binary_missing`,
  `model_missing`, `empty_input`, `ragged_input`, `invalid_rate`, `invalid_range` ; codes de run :
  `cancelled`, `io_error`, `tool_failed`, `no_output`, `unsupported_platform`.
- Aucun appel UI ni `MessageManager` : appelables depuis n'importe quel thread worker.
- Les fenêtres lisent la plage (comme avant, sur leur worker), appellent le service, puis font le
  commit sur le thread de messages exactement comme avant (mêmes textes de statut, progression,
  messages d'erreur et dialogues de détail, annulation, nettoyage du dossier temporaire).
- Toujours Windows-only (`acpp::run*` sont des stubs ailleurs) : `run()` valide puis renvoie
  `unsupported_platform` sur macOS / Linux ; `validate()` est multiplateforme.

## Fichiers

Nouveaux : `include/AI/StemService.h`, `src/AI/StemService.cpp`, `include/AI/SuperResService.h`,
`src/AI/SuperResService.cpp`, `tests/regression/hardening.inc`, ce fichier.
Modifiés : `include/PythonScripts.h`, `src/PythonScripts.cpp`, `src/Application.cpp`
(`runPythonScript` seulement), `src/AudioCpp.cpp` (`runProcessWithPipes` seulement),
`src/StemSeparation.cpp`, `src/SuperResolution.cpp`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`,
`tests/UnitTests.cpp` (une ligne `#include` + une ligne `SC_RUN_TEST`).

## Tests

`tests/regression/hardening.inc`, test « regression: hardening » (dans la suite `--run-unit-tests`
lancée par ctest) :

- `result.json` : document complet, base de temps input/document, `null` au premier niveau,
  15 documents invalides refusés, fichier absent (= pas de résultat), lecture disque UTF-8 ;
- `meta.json` : chemins d'échange + contrat présents, appel historique inchangé ;
- hash : vecteur FIPS « abc » sur les octets, stable, change avec le contenu, fichier absent ;
- liste blanche : dans le dossier, sous-dossier, hors dossier, non-`.py`, absent, lien symbolique
  vers l'extérieur, lien `.py` vers un non-script (skip si les liens ne peuvent être créés : Windows
  sans privilège) ;
- services : chaque code de validation, ancres relatives, `run()` depuis un `std::thread` (Windows :
  atteint la CLI avec un faux binaire → `tool_failed` et dossier de travail conservé ; ailleurs :
  `unsupported_platform`) ;
- processus : exécutable absent = non démarré (tous OS), décodage (UTF-8 invalide, NUL) ; avec
  Python : timeout d'un script qui dort, **petit-enfant qui ignore SIGTERM tué avec l'arbre**,
  annulation, stdin = EOF, `PYTHONUTF8=1` + stdout UTF-8, chemin avec espaces/accents/CJK aller-
  retour dans le script, stderr UTF-8 ;
- `Application::runPythonScript` : script d'analyse seul (`result.json`, pas d'undo, marqueurs
  remappés au document, hash rapporté), exemple `Threshold.py` inchangé, `result.json` invalide,
  limite de temps via la capture, imbrication / thread-local de la capture.
- Les parties qui exigent Python sont sautées (`scSkip`) sans interpréteur, comme les tests Python
  existants.

Résultats locaux (macOS arm64, Debug, `-DSPECTRO_ENABLE_AI=OFF -DSPECTRO_ENABLE_FFMPEG=OFF`,
`ctest -j 6`) :

- référence (branche intacte) : 4614/4615 — échec `views#W4` (repaint 5 px) ;
- après : 4725/4726 — même échec `views#W4`, préexistant et indépendant (passe seul :
  `SC_TEST_FILTER="regression: views"` → 33/33) ; « regression: hardening » : toutes les
  vérifications passent.
- Remarque : sur une machine où **plusieurs checkouts lancent ctest en même temps**, les shards
  partagent `$TMPDIR/SpectroCraft/SpectroCraftTests-tmp-shardN` et s'effacent mutuellement des
  fichiers (échecs aléatoires dans des tests sans rapport). Lancer avec un `TMPDIR` privé.

CI (`gh workflow run build.yml --ref ai-control-hardening`) : voir le rapport final de l'agent
(résultats par plateforme).

## Écarts

- **Base** : `76de144` (= `ai-control` sur le fork) et non `5a80afd` demandé : `5a80afd` est une
  version locale antérieure du même commit (seul `DECISIONS.md` diffère) ; partir de `76de144` évite
  une histoire divergente avec `ai-control`.
- **`tests/CMakeLists.txt`** modifié (hors liste) : la cible console `SpectroCraftTests` compile
  `StemSeparation.cpp` / `SuperResolution.cpp`, qui appellent maintenant les services ; sans les
  nouveaux `.cpp` (et `Sha256.cpp`), elle ne se lierait plus.
- **Correctif embarqué (stems)** : l'ancien code calculait la longueur de bourrage **avant** de
  rééchantillonner les pistes au rate source, ce qui tronquait d'environ 8 % les stems htdemucs
  (44,1 kHz) d'une source à 48 kHz (et ajoutait du silence dans l'autre sens). Le service bourre
  après rééchantillonnage.
- **`killTreeOnExit` vrai par défaut**, aussi pour la GUI : un script qui laisse volontairement un
  processus tourner (lecteur, visionneuse lancé par `subprocess.Popen`) le verra tué à sa sortie.
  Mettre l'option à faux dans une capture si c'est voulu.
- Le panneau Python (GUI) **n'affiche pas** encore `result.json` : un script d'analyse seul réussit
  sans effet visible (et `MainWindow` reconstruit inutilement les mipmaps). Le texte d'aide et le
  gabarit le disent. `MainWindow.cpp` n'a pas été touché (hors périmètre).
- `include/Application.h` n'a pas été touché : son commentaire de `runPythonScript` dit encore que
  le script « doit » écrire le WAV de sortie (à mettre à jour à la fusion).
- Dans un batch d'actions sous une seule capture, seul le rapport du dernier pas Python survit.

## À faire à la fusion (agent principal)

1. **`python.run`** (job `LongRunning`, mutant) :
   - `isUnderScriptFolder(script, &reason, scriptFolder(), &resolved)` ; refus →
     `confirm_required{reason}` (sauf `confirm:true`) ; exécuter **`resolved`** ;
   - sur le worker du job : `sc::py::ScopedScriptRunCapture cap (opts)` avec `opts.timeoutMs` venant
     des `params` (défaut proposé 300 s), puis `app.runPythonScript (resolved, resolved.loadFileAsString (),
     pyCmd, progress, &err)` (le hop vers le thread de messages pour le commit est déjà fait) ;
   - réponse : `cap.report ()` → `scriptSha256`, `exitCode`, `timedOut`, `elapsedMs`, `audioChanged`,
     `outWavPresent`, `result.raw` (ou `markers` avec `docTime/docEnd`, `text`, `measures`, `tables`),
     queues de `stdout/stderr` bornées ;
   - décider si les marqueurs de `result.json` sont ajoutés au document (undoable) ou seulement
     renvoyés (non fait ici) ;
   - `findPython` hors du thread de messages (constat du rapport, `MainWindow.cpp:6334`).
2. **Journal** (`python.run`) : `scriptPath` (résolu), **`scriptSha256`**, `interpreter`, `exitCode`,
   `timedOut`, `elapsedMs`, `audioChanged`, nombre de marqueurs / tables ; **jamais** `text`,
   `measures` ni le contenu des tables sauf `journal_verbose` (§5.3).
3. **`ai.stems.run`** / **`ai.superRes.run`** (jobs, `WindowsOnly`) : construire la `Request` (plage
   lue sur le thread de messages ou via `readRange` comme les fenêtres, modèle résolu avec
   `acpp::listModels` / `listDemucs6Models` / `demucsFtModelDir` / `flashSrModelFile` /
   `listAudioSRModels`), `validate()` → `invalid_params{code}`, `run(req, progressVersJob,
   annulationDuJob)` sur le worker, puis commit sur le thread de messages avec
   `createDocFromMerged` / `applyStemSeparationResult` / `applySuperResolutionResult` (même règle
   « document source changé → nouveau document » que les fenêtres) ; `errorCode` → code d'erreur
   du protocole, `stems[]` dans la réponse si `workDir` est fourni.
4. Mettre à jour le commentaire de `Application::runPythonScript` dans `include/Application.h`
   (`out.wav` facultatif avec `result.json`, capture).
5. Conflits attendus, triviaux : `tests/UnitTests.cpp` (lignes `#include "regression/…"` et
   `SC_RUN_TEST`), `src/CMakeLists.txt` / `tests/CMakeLists.txt` (listes de sources).
