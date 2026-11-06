# Lot 7 — durcissement, pont Python, flags historiques, stems/SR (Windows)

Statut : **fait**, sauf l'exécution réelle des stems/SR (Windows seulement, jamais exécutée ici : CI).
Cœur (runProcess durci, `result.json`, services stems/SR, hash, liste blanche) livré par l'agent
parallèle `ai-control-hardening` (voir `lot-7-core.md`).

## Fusion

`ai-control-hardening` fusionnée à `db710c5` ; conflits triviaux (listes de sources, `UnitTests.cpp`) ;
`Sha256.cpp` apparaissait deux fois dans chaque cible (déplacé hors du bloc IA par les deux agents) :
dédoublonné. Commentaire de `Application::runPythonScript` mis à jour (`out.wav` facultatif avec
`result.json`, capture).

## Commandes (`src/control/commands/Python.cpp`)

- **`python.run {script, timeoutMs (défaut 300 s), addMarkers, select, channels, confirm}`** (alias
  `python`) : `isUnderScriptFolder` (liens résolus) ; hors du dossier des scripts → `confirm_required
  {reason:"outside_script_folder"}` puis périmètre de chemins ; job : `findPython` **sur le worker**,
  `ScopedScriptRunCapture` (timeout qui tue l'arbre de processus, stdin fermé, UTF-8), patch attribué à
  `python.run`/client. Résultat : `scriptSha256`, `interpreter`, `exitCode`, `timedOut`, `elapsedMs`,
  `audioChanged`, `outWavPresent`, `resultJson` (marqueurs en temps document, texte, mesures, tables),
  `markersAdded`, fins de stdout/stderr (4 Kio). Timeout → code `timeout`. Le **journal** garde
  `script`, `scriptSha256`, `interpreter`, `exitCode`, `timedOut`, `audioChanged`, `markerCount`… jamais
  `text`, `measures` ni les tables (liste blanche dans `ControlState`).
- **`ai.stems.run {mode samAudio|htdemucs6|htdemucsFt, descriptions[{text, anchors}], model, backend,
  extractFromOriginal, output newDoc|replace|append, select, channels}`** et **`ai.superRes.run {mode
  flashsr|audiosr, model, backend, output, …}`** : `WindowsOnly` (ailleurs `unsupported_platform`) ;
  plage lue sur le thread de messages, `validate()` → `invalid_params`/`model_not_installed {code}`,
  `run()` sur le worker avec progression/annulation du job, commit sur le thread de messages par
  `applyStemSeparationResult` / `applySuperResolutionResult` avec la règle des fenêtres (document
  modifié entre-temps → nouveau document). `backend` par défaut = `ai.setDevice {engine:"audiocpp"}`.
  **Non exécutées** ici (macOS) : seule la compilation et le refus `unsupported_platform` sont testés ;
  le CI Windows compile le chemin complet.

## Flags historiques

Codes de retour fiables pour `--test-spectral`, `--test-load`, `--test-noise-suppression`,
`--test-transcribe`, `--test-batch-transcribe`, `--test-ns-failure-alert` : 2 = usage, 1 = échec
(ouverture, lecture, pipeline, installation de modèle, transcription, au moins un fichier en échec du
batch), 0 sinon ; `--test-batch-transcribe` tourne sur un **magasin de réglages isolé** (il réécrivait le
vrai `SpectroCraft.recent`). (Le rapport proposait de les réimplémenter sur `--json` : inutile une fois
les codes corrigés, `--json` couvrant le reste ; ils restent pour compatibilité.)

## Revue de sécurité (liste fermée du §8)

| Point | État |
|---|---|
| Périmètre `control_roots` et liens | POSIX : `realpath` sur chaque composant existant ✓ ; Windows : dernier composant seulement (dette) ; `control_roots`/`control_enabled` **non modifiables par l'API** (`settings.set` refuse) ✓ |
| Jeton | **remplacé** : 256 bits du CSPRNG de l'OS (`std::random_device`) au lieu de `juce::Uuid` (générateur non cryptographique) ; comparaison à temps constant ; jamais journalisé ni loggé (`sanitizedParams`, le hello n'est pas journalisé) ; Windows : ACL par défaut du profil (pas d'ACL explicite, modèle de menace §5.2) |
| Limites | ligne 1 Mio, params 256 Kio, 4 Kio et profondeur JSON ≤ 64 avant authentification, 10 s pour s'authentifier, 4 clients, file d'événements 1000 + `events.dropped`, envoi borné à 5 s ✓ ; générateurs et `doc.new` refusés au-delà de 4 Gio d'audio (le fuzz a montré qu'on pouvait sinon demander des téraoctets) |
| Contenu du journal | métadonnées seulement ; pas de texte de transcription (test lot 3), pas de texte/mesures Python (test), pas de valeurs de tables ✓ |
| Exécution de scripts | dossier des scripts + `confirm` + hash SHA-256 journalisé + timeout ✓ ; l'étape `python` de `step.run` vérifie dossier/racines/`confirm` mais tourne sans la capture (pas de timeout) : utiliser `python.run` |
| Binaires téléchargés | SHA-256 obligatoire avant `installable:true` (catalogue) ; CUDA bloqués (DLL manquantes, préexistant) ✓ |
| `multi_instance=0` | `--control` saute `handleStartup` ✓ |
| **Fuzz** 10 000 lignes NDJSON aléatoires | **a trouvé un crash** : `juce::var::isObject()` est vrai pour un **tableau**, `getDynamicObject()` renvoyait alors nul (`"params":[]` → segfault, déni de service par un client authentifié ou une ligne `--json`). Corrigé partout (`isObj()` = `getDynamicObject() != nullptr`) ; le fuzz passe ✓ |
| **Charge** 1000 `app.state` séquentiels par le vrai socket | **253 ms au total, p50 0,24 ms, p99 0,35 ms** (seuils : < 10 s, p99 < 50 ms) ✓ ; la stabilité mémoire sur 10 000 commandes n'a pas été mesurée (pas d'outil de mesure fiable dans la suite) |

## Tests (`tests/regression/lot7_control.inc` ; « control load » est sériel)

`python.run` (script d'analyse : hash, `result.json`, marqueur ajouté en temps document, audio intact,
mesures renvoyées, journal avec hash sans texte ; script hors dossier → `confirm_required` ; script qui
dort → `timeout` en < 15 s), stems/SR (`unsupported_platform` hors Windows, validation sous Windows),
fuzz 10 000 lignes (toutes répondues, app vivante), charge.

Résultats : voir en bas.

## Dettes

- Stems/SR : jamais exécutés ici ; les binaires CUDA restent bloqués (bug préexistant d'extraction des
  DLL, signalé par l'agent du catalogue) ; les dialogues les proposent encore.
- Pas de mesure de dérive mémoire sur 10 000 commandes.
- `step.run {type:"python"}` sans timeout (préférer `python.run`).

## Résultats

macOS arm64, Debug + IA, Python 3 présent : « lot7_control + control load + contrôle + menus » 1161/1161 ;
suite complète **6571/6571**, 10/10. Charge (après passage au jeton CSPRNG) : 1000 `app.state` en
252 ms, p50 0,24 ms, p99 0,32 ms. Fuzz : 10 000 lignes en ~1 min, aucune perte, app vivante.
