# Lot 0 — registre minimal, `step.run`, journal de session, `--json`, correctifs Actions

Statut : **fait** (macOS arm64, build Debug avec IA). Linux/Windows : voir le CI (note en fin de fichier).

## Commits

1. `f1cfa62` — *Fix action-step replay defects and worker-side AI commits* : **uniquement** les correctifs
   indépendants, livrables à SuperMax3d en PR séparée (Q12) :
   - `applyActionStep` : `case 'e'` (`envelope`) ajouté ;
   - étape `noiseSuppress` : `device` (param optionnel, sinon `app.aiDevicePref`) résolu par
     `sc::resolveAiDevice` contre `OvRuntime::devices()` au lieu du littéral `"auto"` ; `df3PostFilter`,
     `copyNoise`, `autoDownload` honorés ;
   - build sans IA : `noiseSuppress` échoue (« no AI support ») au lieu d'exécuter `normalize` ;
   - dispatch durci : type inconnu rejeté avant le `switch`, chaque `case` compare le nom complet
     (« sxxx » lançait `silence`) ;
   - `Application::transcribe` et la branche par défaut de `Application::noiseSuppress` committent sur le
     thread de messages (`callAsync` + `WaitableEvent`, helper `commitOnMessageThread`), comme la branche
     `copyNoise` et `runPythonScript`. La transcription n'est stockée que si le document est encore ouvert.
   - Tests : `testApplyActionSteps` (envelope, types sosies, NS sans IA) et
     `testRegAiNoiseSuppressCommitHop` (`regression/ai.inc`) qui **prouve** l'attente du thread de messages
     (worker bloqué tant que le thread de messages ne pompe pas) et rejoue NS avec `device:"auto"`.
2. Commit du lot 0 proprement dit (registre, commandes, `--json`, tests) — voir `git log`.

## Ce qui a été fait

Nouveaux fichiers (`include/control/`, `src/control/`) :

| Fichier | Rôle |
|---|---|
| `CommandRegistry.h/.cpp` | `Spec {id, title, category, params (ParamSpec), flags, aliases, handler}`, `ParamSpec` → validation **et** JSON Schema draft 2020-12 restreint (une seule source), `Request::parse`, `invoke()` (ordre du §5.2 : résolution du doc → drapeaux → garde busy → bascule de doc temporaire → `ScopedCapture` → handler → journal → `refreshAfterEdit` → enveloppe) |
| `ControlState.h/.cpp` | état par `Application` : `Journal`, `JobManager`, `UiBridge*`, puits d'événements ; `Application::control()` (créé à la demande, arrêté en premier par `~Application`) |
| `Journal.h/.cpp` | anneau 10 000 entrées, `seq` monotone, `kind ∈ command|step|history|job`, thread-safe, miroir JSONL prévu (lot 5) |
| `JobManager.h/.cpp` | jobs `j-<n>` (un `std::thread` chacun, jamais détaché, joint à l'arrêt), un seul mutant à la fois, progression throttlée, `JobControl::onMessageThread` (commit par `callAsync`, attente bornée : abandon à l'arrêt) |
| `UiBridge.h` | interface (implémentée par `MainWindow` au lot 1 ; `nullptr` en CLI/tests) |
| `StateJson.h/.cpp` | `app.state`, résumé de doc, `history.list` (sémantique `HistoryPanel`, jamais `oldData/newData`), `helloInfo`, version depuis `Version.h` |
| `JsonCli.h/.cpp` | `--json <f|->`, `--json-out`, `--use-user-settings`, `--list-commands [--markdown]` ; session ligne à ligne (`runJsonSession`, testable sans processus) |
| `commands/Core.cpp` | `app.describe`, `app.state`, `doc.list/open/activate/meta/saveAs/save`, `selection.set/all`, `journal.read`, `debug.sleep` |
| `commands/History.cpp` | `history.list/undo/redo/jumpTo` (échec d'`applyPatch` → `failed {historyCleared:true}`) |
| `commands/Steps.cpp` | `step.run`, `steps.run`, `step.validate` (les 20 types, `select` explicite, `channels[]`) |
| `commands/Jobs.cpp` | `job.get/cancel/list` (annulation réservée au client lanceur sauf `force`) |

Modifications : `FileDoc::id` (id stable, atomique) ; `AlertGate::ScopedCapture` (profondeur + liste capturée,
ne touche jamais au drapeau headless) ; `Application::recordActionStep` journalise **avant** la garde
`actionRec.active` ; `undoSteps/redoSteps` journalisent undo/redo/vidage ; `Application::saveDocument`
(+ `beginSave/completeSave`, logique extraite de `MainWindow::runSaveAsync`) ; `Main.cpp` : branche
`--json/--list-commands`, **flag inconnu → code 2** (avant : ignoré, l'UI démarrait) ; CMake :
`SC_CONTROL_SOURCES` partagé par l'app et `SpectroCraftTests`.

## Tests ajoutés (`tests/regression/control.inc`, « regression: control »)

- `testControlRegistry` : les **20** `actionTypes` passent `step.validate` puis `step.run` sur une plage
  explicite et un canal (IA : `denseunet` avec `device:"auto"`, `whisper-base`, Python : `Threshold.py`
  — skip explicite si modèle/interpréteur absent ; sans IA : `unsupported_build`).
- `testControlDocsHistory` : `no_document`, chemins relatifs refusés, ids stables, `unknown_doc`,
  **édition d'un document non actif** (`docSwitched`, actif et sélection restaurés), `selection.set` sur doc
  non actif, `history.list` (jointure opId/params/source), undo/redo/jumpTo (pile redo conservée),
  `redoDiscarded`, `doc.saveAs` (`confirm_required` puis écriture), `doc.save` exige `confirm`, `doc.meta`,
  journal (commandes, étapes, undo/redo, source `menu`), `ScopedCapture`, `busy` pendant un job mutant,
  `job.cancel`, `job_not_found`, arrêt des jobs < 2 s à la destruction.
- `testControlDescribe` : chaque commande a un schéma objet fermé ; ses `params` minimaux générés passent la
  validation ; chaque `invalid_params` cite un `path` présent dans le schéma ; contrôles min/max/enum/entier/
  items/plage.
- `testControlJsonCli` : streaming ligne à ligne (chaque ligne répondue avant lecture de la suivante),
  codes 0/1/2, `continueOnError`, JSON illisible → 2, ligne > 1 Mio → 2, **timeout d'un LongRunning**
  (`debug.sleep` annulé et rapporté `timeout` en < 5 s) ; puis l'**exécutable réel** (`--json fichier
  --json-out fichier` avec espaces dans le chemin, codes 0/1/2, flag inconnu → 2, `--list-commands --markdown`).

## Résultats (macOS arm64, Debug + IA, `ctest -j 8`)

- « regression: control » seul : **431/431** (avec `apply action steps`), aucun skip (modèles et Python présents).
- Suite complète : **5283/5284** checks (référence : 4859/4859). L'unique échec,
  `single instance mode: empty list acknowledged`, est le délai fixe de 2 s du tube FileForward sous la
  charge de la machine (cinq agents compilent et testent en parallèle sur ce Mac) ; il passe 5 fois sur 5
  seul, ne touche aucun code modifié ici, et était déjà apparu une fois sur l'arbre des seuls correctifs.
- **Correctif d'infrastructure de test** : le dossier temporaire de la suite s'appelait
  `SpectroCraftTests-tmp-shard<k>` pour **tous** les processus de la machine ; deux checkouts testés en
  même temps (ici : les autres agents) s'effaçaient mutuellement leurs fichiers en cours de run (échecs
  « file loaded », « doc.open », segfault d'un shard). Il porte maintenant le PID
  (`...-shard<k>-<pid>`, `tests/UnitTests.cpp`, `scTestTempDir`).

## Écarts par rapport au plan

- **JobManager avancé au lot 0** (prévu au lot 1) : sans lui, impossible de tester « timeout d'un
  LongRunning » exigé par `testControlJsonCli`. Une commande de test `debug.sleep` (catégorie `debug`,
  drapeau `Internal`, cachée de `app.describe` par défaut) sert de job témoin.
- `doc.save` (en place, `confirm` obligatoire) ajouté dès le lot 0 à côté de `doc.saveAs` (même code).
- `doc.activate`, `job.get/cancel/list` ajoutés dès le lot 0 (bon marché, utiles au CLI).
- `history.list` : avant le lot 5, `opId/params/source/ts` viennent d'une **jointure best-effort** avec le
  journal (profondeur undo + description) ; elle se trompe si la pile a été tronquée à 64 niveaux.
- La liste des états futurs est dans l'**ordre de redo** (le prochain redo d'abord). Constat : le
  History panel (`HistoryPanel::refresh`, `src/Panels.cpp:920-925`) liste `redoAt(0..n)`, c'est-à-dire
  l'ordre inverse : la première ligne « future » affichée n'est pas celle que rejoue un clic dessus.
  Non corrigé ici (UI, à confirmer avec Max) ; à traiter au lot 5 qui retouche ce panneau.
- Pas de build Linux local (machine macOS, voir `baseline-macos.md`).

## Dettes / limites connues

- `step.run` est **synchrone** au lot 0 : en GUI, un `noiseSuppress`/`transcribe`/`python` long bloquerait
  le thread de messages (le serveur du lot 1 répondrait `timeout`). Le lot 1 convertit ces types en jobs.
- Pas encore de détour « fichier streamé → job » ni de périmètre de chemins (`control_roots`) : lot 1.
- `recordActionStep` est appelé par les menus **avant** l'édition : une édition qui échoue laisse une
  entrée `step` dans le journal.
- Le journal n'a pas encore de fichier JSONL (lot 5).

## Consignes pour la suite

- Lot 1 : brancher `ControlState::eventSink` et `Journal::onAppend` sur le serveur ; implémenter `UiBridge`
  dans `MainWindow` ; convertir les types longs de `step.run` et le détour streamé en jobs via
  `JobManager::start` + `JobControl::onMessageThread` ; ajouter le périmètre de chemins dans `doc.open`,
  `doc.saveAs`, `python`.
- Toute nouvelle commande : un `Spec` avec ses `ParamSpec` (le schéma suit) et, si lecture pure, le
  drapeau `Query` (non journalisée en cas de succès).
