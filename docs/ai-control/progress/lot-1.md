# Lot 1 — serveur de contrôle dans l'app ouverte + jobs

Statut : **fait** (macOS arm64). Voir « Résultats » et « CI » en fin de fichier.

## Ce qui a été fait

### Serveur (`include/control/ControlServer.h`, `src/control/ControlServer.cpp`)

- `juce::Thread "SC-Control"` : `createListener (0, "127.0.0.1")` + `getBoundPort()`, boucle d'acceptation
  `waitUntilReady (true, 500)` ; un thread `SC-ControlClient-<n>` par client, **4 clients max** (le 5ᵉ reçoit
  `busy` puis est fermé).
- NDJSON : découpage sur `\n` (CRLF toléré), ligne > 1 Mio → `invalid_params` et la session continue,
  JSON illisible → `invalid_params`.
- **Authentification** : `session.hello {token, client, subscribe[], sinceSeq}` obligatoire en premier
  (comparaison à temps constant) ; 3 échecs → pause de 2 s ; `unauthorized` sinon. Jeton = 2 `Uuid`
  (64 hex), jamais journalisé (`sanitizedParams` retire `token`), seulement dans le fichier de découverte.
- **Le thread réseau ne touche jamais `Application`** : chaque requête est postée par `callAsync` avec un
  `PendingCall` partagé (`WaitableEvent` + `abandoned`) et attendue **au plus `timeoutMs`** (défaut 30 s) →
  `timeout {hint: "a modal dialog is probably open"}` ; pendant l'attente, la file d'événements est vidée
  toutes les 20 ms. À l'arrêt, attente abandonnée → `shutting_down`.
- **Arrêt** (`stop()`) : `shuttingDown`, **auto-connexion au port réel** avant `listener->close()`
  (JUCE se connecte au port 0 sur Linux, §5.2), `stopThread (2000)`, sessions fermées puis jointes,
  fichier de découverte supprimé. Appelé dans `shutdown()` **avant** `mainWindow.reset()`.
- **Fichier de découverte** `<userApplicationDataDirectory>/SpectroCraft/control/<pid>.json`
  `{pid, port, token, version, protocol, startedAt, exe, platform}` : dossier 0700, fichier créé vide,
  passé en **0600**, puis écrit (POSIX) ; balayage des fichiers dont le PID est mort au démarrage
  (`kill(pid,0)` / `OpenProcess`).
- **Événements** : chaque entrée de journal → `journal.entry` (abonnement `journal`) et, selon le type,
  `history.changed` (undo/redo, commandes et jobs mutants, éditions menu) ; `job.progress`/`job.done`
  (JobManager) ; `doc.changed` (commandes doc/selection/view/markers). File bornée à 1000 par client,
  `events.dropped {count}` au-delà. `events.subscribe {kinds, sinceSeq}` et `session.hello {sinceSeq}`
  **rejouent** le journal en mémoire (le rejeu complet/persistant est au lot 5).
- `session.bye` ferme proprement.

### Application / fenêtre

- Réglage `control_enabled` (défaut 0), menu **Window > Allow AI control (localhost)** (démarre/arrête le
  serveur à chaud), flag **`--control`** (démarre le serveur et **saute `handleStartup`** : jamais de
  forward vers une instance existante, même avec `multi_instance=0`, Q15).
- Option CMake **`SC_ENABLE_CONTROL`** (ON, Q14) : à OFF, plus de serveur, de `--control` ni d'item de menu
  (le registre et `--json` restent).
- Indicateur « AI control: N client(s) » dans la barre d'état (`StatusBar::setControlText`).
- `MainWindow` implémente `sc::control::UiBridge` (rafraîchissement, fenêtre temporelle, zooms, vues,
  panneaux, dialogue de progression non bloquant des jobs, quit) et s'enregistre dans `app.control().ui`.
- `MainWindow::runSaveAsync` passe désormais par `Application::beginSave/completeSave` (un seul chemin de
  sauvegarde pour le menu et l'API).

### Jobs

- `startJob (ctx, …)` : avec fenêtre, ouvre le même `WorkProgressDialog` que les menus (Annuler compris →
  annule le job) ; `isBackgroundJobRunning()` et le refus de quitter le voient sans modification.
- Deviennent des jobs : `step.run`/`steps.run` contenant `noiseSuppress`, `transcribe` ou `python`
  (le worker n'édite jamais le document : ces méthodes committent via le thread de messages ; les autres
  étapes d'une séquence passent par `JobControl::onMessageThread`) ; `doc.open` > 200 Mo (FileDoc chargé sur
  le worker, inséré sur le thread de messages) ; `doc.save/saveAs` dans l'app ouverte (écriture audio sur le
  worker, `completeSave` sur le thread de messages) ; **tout `Mutates` sur un fichier streamé** (détour
  §5.2 étape 4 : `ensureEditable` sur le worker puis la commande, résultat `materialised:true`).
- Un job sur un document non actif rend ce document actif durablement (`activated:true`), puisque le
  worker travaille sur le document actif.
- Pendant un job mutant, toute commande non `Query` (hors `job.*`) répond `busy`.

### Nouvelles commandes

`view.set`, `view.zoom {factor|to|vertical}`, `view.channels`, `view.show` (NeedsUi), `ui.panels`,
`ui.panel.set` (NeedsUi), `transport.play/stop/pause/loop/playSpectralSel` (`ensureAudioReady (2000)`,
jamais l'attente infinie de `play()`), `markers.add/list`, `doc.close {confirm}`, `doc.closeAll {confirm}`,
`app.quit {confirm}` (NeedsUi ; refusé si un job tourne).

### Sécurité (§5.3)

- Loopback seulement, jeton, opt-in, pause après 3 échecs, 4 clients, lignes ≤ 1 Mio, params ≤ 256 Kio,
  file d'événements bornée.
- **Périmètre de chemins** (`include/control/PathPolicy.h`) : racines = réglage `control_roots`
  (séparateur `;`) ou, par défaut, dossier personnel + `scriptFolder()` + dossier temporaire ; liens
  symboliques résolus sur chaque composant existant (`realpath`, POSIX) ; hors racines →
  `confirm_required {reason:"outside_roots"}` ; dossier `control/` et `SpectroCraft.recent` refusés même
  avec `confirm`. Appliqué à `doc.open`, `doc.saveAs`, et aux scripts des étapes `python`, qui exigent en
  plus `confirm` hors de `scriptFolder()`.

## Tests (`tests/regression/control_server.inc`, test sériel « control server »)

- serveur : balayage d'un `<pid>.json` forgé (PID mort), fichier de découverte `{pid, port, token,
  protocol}` en 0600, `unauthorized` avant hello, mauvais jeton, **pause ≥ 1,5 s après 3 échecs**,
  hello (protocole, version), `doc.open`, `step.run` sur doc non actif (`docSwitched`), événement
  `history.changed`, job + `job.done`, ligne > 1 Mio puis session encore utilisable, `unknown_cmd`, JSON
  illisible ; **thread de messages bloqué → `timeout`** (attente bornée) ; 4 clients acceptés, 5ᵉ `busy` ;
  **arrêt avec un client connecté et une requête en vol** : `stop()` < 2 s, réponse `shutting_down`,
  fichier supprimé, port fermé ; redémarrage.
- périmètre de chemins : temp autorisé, hors racines → `confirm_required`, `confirm` accepté, dossier de
  contrôle et `.recent` refusés, `doc.saveAs` hors racines, lien symbolique sortant résolu.
- vue/transport/marqueurs/fermeture en headless (`needs_ui` pour vertical/show/quit), **fichier streamé →
  job qui le matérialise puis applique l'étape**.

## Revue indépendante (sous-agent relecteur, lecture seule) et correctifs appliqués

Une relecture adversariale des lots 0-1 a relevé, puis j'ai corrigé dans ce lot :

1. **Garde busy trop étroite** : seules les commandes `Mutates|LongRunning` étaient refusées pendant un
   dialogue modal / une opération de fond. JUCE délivre les `callAsync` dans les boucles modales : un
   `doc.close` pouvait libérer le document qu'écrit le thread de `runSaveAsync` (use-after-free). Désormais
   **toute commande non `Query`** (hors `job.*`) répond `busy` dans ces cas.
2. **`app.quit`** contournait le refus « job en cours » : `requestQuit (discardUnsaved)` revérifie, au moment
   de quitter, `isBackgroundJobRunning()`, les jobs API et les documents non sauvés.
3. **Crash avant authentification** : le parseur JSON de JUCE est récursif ; une ligne de 100 000 `[`
   aurait fait déborder la pile du thread de session. Profondeur limitée à 64 (pré-scan) ; avant
   `session.hello`, lignes ≤ 4 Kio et **10 s** pour s'authentifier (sinon fermeture : les 4 places ne
   peuvent plus être squattées).
4. Étapes exécutées dans un job journalisées `source:"menu"` → `ScopedInvokeSource` autour des commits.
5. `history.list` attribuait la dernière édition à `doc.save`/`history.undo` : `undoDepth/description`
   ne sont journalisés que si le contenu a réellement changé (`contentVersion`), hors catégorie `history`
   (idem pour les entrées de fin de job).
6. `transport.*` et `view.*` sur un document non actif jouaient/affichaient le mauvais document : drapeau
   `ActiveDocOnly` → `invalid_params {hint: doc.activate}`.
7. `send()` côté serveur borné à 5 s et interrompu par l'arrêt.
8. `session.hello` : réponse unique même en cas d'échec, authentification **après** la réponse, aucun
   événement avant elle, rejeu ensuite.
9. Ordre des options de `--json` libre (`--json-out` avant `--json` donnait « unknown flag »).

Suspicions non corrigées (documentées) : le worker d'un job de sauvegarde ou du détour streamé lit/écrit
l'`AudioEngine` pendant que des `Query` (`app.state`) lisent ses compteurs atomiques — c'est le modèle de
`MainWindow::runDestructiveEdit`/`runSaveAsync`, qui repose sur la synchronisation interne d'`AudioEngine` ;
un arrêt de l'app attend la fin d'une sauvegarde ou d'un téléchargement en cours (pas d'annulation
possible) ; `job.cancel` se fie au nom de client déclaré.

Contraintes du lot 2 (adaptateur MCP) respectées : `seq` des événements = compteur du journal
(`Journal::reserveSeq` pour les événements non journalisés, **un seul événement par entrée**) ;
fichier de découverte écrit après le démarrage du thread d'acceptation, supprimé à l'arrêt ;
`startedAt` ISO-8601 UTC ; FileForward inchangé ; `--control` saute `handleStartup` ; `Query` seulement
sur des lectures pures. Formes alignées sur le §6 à peu de frais : `history.list` porte `doc` **et**
`docId`, l'état 0 `label` **et** `description` ; `doc.open` renvoie `id` **et** `doc`. `patchSeq` :
lot 5.

## Écarts par rapport au plan

- `tools/mcp/sc_client.py` **non écrit ici** : l'agent du lot 2 (`ai-control-mcp`) possède `tools/mcp/**`.
  Le test bout-en-bout manuel a été fait avec un script Python jetable (voir « Résultats »).
- `ClientSession` est dans `ControlServer.cpp` (pas de `ClientSession.cpp` séparé).
- Requêtes d'un même client traitées **séquentiellement** (pas de pipeline de 64 requêtes en vol) : la
  limite de 64 est donc trivialement respectée ; un client qui veut du parallélisme ouvre une 2ᵉ connexion.
- `transport.play` sans périphérique (CI, `SC_NO_AUDIO_DEVICE`) réussit en silence (`deviceDisabled:true`)
  au lieu d'échouer : c'est le comportement de `Application::play()`.
- Windows : la résolution des liens ne porte que sur le dernier composant du chemin.

## Dettes / limites

- Le rejeu d'événements ne couvre que l'anneau mémoire (10 000 entrées) ; persistance au lot 5.
- L'abandon d'une requête après `timeout` n'annule pas son exécution : elle peut encore s'exécuter quand
  le thread de messages se libère (le client doit relire `app.state`).
- Pas d'ACL explicite sur Windows pour le fichier de découverte (profil utilisateur, modèle de menace
  retenu au §5.2).

## Résultats (macOS arm64, Debug + IA)

- « control server » : 67/67 (sériel), « regression: control » + serveur : 655/655 avec les tests voisins.
- Suite complète `ctest -j 8` : **5482/5482**, 10/10 (référence 4859, lot 0 5284).
- **Bout en bout réel** (script Python jetable, app lancée avec `--control`, vraie fenêtre) : découverte
  (`~/Library/SpectroCraft/control/<pid>.json`), `session.hello`, `doc.open`, `step.run fadeOut` avec
  événement `history.changed` (source `api:smoke`), `history.list`, `view.zoom {vertical}` (NeedsUi),
  `ui.panels`, `history.undo`, `app.state`, `app.quit {confirm}` → l'app quitte (code 0) et supprime son
  fichier. Un fichier orphelin laissé par un `kill` a bien été balayé au lancement suivant.
- Ce test a montré que le dossier temporaire de JUCE sur macOS est `$TMPDIR/SpectroCraft` : les racines
  par défaut incluent donc aussi `$TMPDIR` et `/tmp` (POSIX).
- Linux : correctif de portabilité `int64_t` → `juce::int64` (le CI `linux-ai` du lot 0 avait échoué sur
  `contentVersion()` : `long` ambigu pour `juce::var`) ; macOS et Windows du lot 0 étaient verts.

## Consignes pour la suite

- Lot 2 : le protocole réel est celui de `src/control` (enveloppe §6) ; `session.hello` renvoie
  `{app, version, pid, protocol, platform, capabilities, commands, journalSeq, client, replayed}`.
  Événements : `{"event", "seq", "ts", "data"}`.
- Toute commande longue : `startJob` + `JobControl::onMessageThread` pour le commit.
