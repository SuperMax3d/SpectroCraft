# Lot 2 — adaptateur MCP Python + intégration Claude Code

Statut : **fait côté Python, testé** ; bout en bout contre l'app **ouverte** impossible tant que le
serveur de contrôle (lot 1) n'existe pas. Branche `ai-control-mcp` du fork, partie de `ai-control` =
`76de144` (le `5a80afd` cité dans la consigne est la version locale, non poussée, du même commit de docs :
seule une phrase de `DECISIONS.md` diffère ; `76de144` est celui du fork, donc fusion sans conflit).

## Ce qui a été fait

| Fichier | Rôle |
|---|---|
| `tools/mcp/pyproject.toml` | paquet `spectrocraft-mcp`, script `spectrocraft-mcp = spectrocraft_mcp.server:main`, `mcp>=1.6,<2`, extra `[test]` (pytest, jsonschema) ; marche avec `uv run --directory tools/mcp spectrocraft-mcp` et `pip install -e tools/mcp` (les deux vérifiés) |
| `spectrocraft_mcp/protocol.py` | constantes (protocole 1, ligne ≤ 1 Mio, 30 s), codes d'erreur fermés §6.1, `ControlError.describe()` (code, message, `details.hint`, marche à suivre par code, détails) |
| `spectrocraft_mcp/discovery.py` | `<appData>/SpectroCraft/control/*.json` aux vrais chemins (macOS `~/Library/…`, Linux `$XDG_CONFIG_HOME` ou `~/.config/…`, Windows `%APPDATA%\…`) ; supprime les fichiers dont le PID est mort ; **PID réutilisé** : fichier écarté si le processus vivant a démarré plus de 5 s après `startedAt` (heure de création via `/proc` sous Linux, `ps -o lstart` sous macOS, `GetProcessTimes` sous Windows) ; choix `SPECTROCRAFT_PID` sinon le plus récent ; le jeton n'apparaît jamais dans les sorties |
| `spectrocraft_mcp/client.py` | client bloquant thread-safe : `session.hello {token, client, subscribe, sinceSeq}`, thread lecteur qui route par `id` (réponses dans le désordre OK) et met les événements en file (bornée 1000), `call()`/`result()`/`run()`, `wait_job(job, on_progress)` (événements `job.progress`/`job.done` **plus** sondage `job.get` toutes les 2 s, donc robuste si l'app n'émet pas encore d'événements), **reconnexion silencieuse** au prochain appel avec rejeu `sinceSeq` (remis à zéro si c'est un autre processus), limite 1 Mio dans les deux sens ; une requête non envoyée est renvoyée après reconnexion, une requête perdue en vol n'est renvoyée que si la commande est en lecture seule (`Query` de `app.describe`) |
| `spectrocraft_mcp/dynamic.py` | outils dynamiques depuis `app.describe` (schéma `params` inchangé + `doc`, `timeoutMs`, `confirm` optionnels ; renommage `sc_doc` si une commande a déjà un param `doc`) |
| `spectrocraft_mcp/pipe_probe.py` | sonde du tube `SpectroCraft.FileForward` (`int32 0` → ack `0x01`) : FIFOs `/tmp/SpectroCraft.FileForward_in/_out` (POSIX, FIFO orphelin → « personne ») et `\\.\pipe\SpectroCraft.FileForward` (Windows) |
| `spectrocraft_mcp/launcher.py` | `resolve_exe` (bundle `.app` accepté), `launch()` (sonde d'abord, puis `SPECTROCRAFT_EXE --control`, stdin=DEVNULL, détaché, attente ≤ 20 s du `<pid>.json`), `run_headless()` (`--json - --json-out <tmp>`, ids `h1…`, `continueOnError`, code de retour, fin de stderr) |
| `spectrocraft_mcp/server.py` | FastMCP : les 24 outils fixes du §7.3, outils dynamiques, ressources `spectrocraft://commands` et `spectrocraft://journal/tail`, progression relayée par `ctx.report_progress`, erreurs de l'app → erreurs d'outil MCP (`isError`) avec code + indice + marche à suivre, politique `SPECTROCRAFT_LAUNCH` (`ask`/`auto`/`never`), `eventsSinceLastCall` (éditions faites entre-temps à la souris ou par un autre client), notification `tools/list_changed` au premier contact |
| `tools/mcp/sc_client.py` | CLI **sans dépendance** (`python3 -I -S` vérifié) : `--list`, `app.state`, `step.run '{"type":"fadeOut",…}'` (forme plate acceptée), attente des jobs avec progression sur stderr, `--events N`, codes 0/1/2 |
| `tools/mcp/smoke.sh` | bout en bout local contre l'app ouverte (`--control`) : `app.state`, `doc.open`, `step.run`, `history.list`, `history.undo` |
| `.mcp.json.example`, `.gitignore` | gabarit macOS du §7.4 ; `.mcp.json`, `tools/mcp/.venv`, `uv.lock`, caches ignorés |
| `CLAUDE.md` | consignes à Claude Code : outils, vocabulaire (docId, select, channels, jobs, confirm, unités), build Ninja Debug, ctest, `SC_TEST_FILTER`, tests Python |
| `docs/ai-control/mcp.md` | installation macOS/Windows/Linux, variantes `uv`/`pip`, variables, dépannage |

Tests (`tools/mcp/tests`, pytest) : faux serveur NDJSON en processus (`fake_server.py` : jeton,
routage par `id`, événements + rejeu `sinceSeq`, jobs avec progression/échec/annulation, codes d'erreur,
coupures provoquées, lignes géantes) et faux exécutable (`fake_exe.py` : `--json` et `--control`).
Fichiers : `test_discovery` (chemins par OS, PID mort, PID réutilisé, `startedAt` ISO/s/ms, fichiers
corrompus), `test_client` (hello, `unauthorized`, routage dans le désordre, 6 codes d'erreur, événements,
`wait_job` avec/sans événements, échec, annulation, timeout, reconnexion + rejeu, app relancée, mutation
en vol non rejouée, lecture rejouée, limites 1 Mio), `test_dynamic` (**chaque `inputSchema` validé par
`jsonschema.Draft202012Validator.check_schema`**, schéma d'origine intact, noms, collisions, schéma
absent ou non-objet, validation d'arguments, `describe_lot0.json` = describe **réel** du lot 0),
`test_pipe_probe` (POSIX), `test_launcher`, `test_server` (client MCP en mémoire ↔ serveur ↔ faux app :
outils fixes/dynamiques, routage, erreurs, progression, ressources, `sc_launch`, `sc_headless`),
`test_stdio` (`python -m spectrocraft_mcp` en vrai stdio), `test_sc_client`, `test_real_exe`
(**skip** sans `SPECTROCRAFT_EXE`).

## Résultats

- macOS arm64, Python 3.11.15, `mcp` 1.30.0 : **98 passed, 4 skipped** (les 4 tests `test_real_exe`) en ~9 s ;
  même résultat via `pip install -e "tools/mcp[test]"` puis `pytest tools/mcp/tests` depuis la racine.
- Avec `SPECTROCRAFT_EXE` = build **réel** de `ai-control` `693f3f3` (lot 0, Release, `-DSPECTRO_ENABLE_AI=OFF`,
  compilé hors dépôt dans un dossier temporaire) : **102 passed** ; scénario `--json` réel vérifié :
  `doc.open` → `step.run fadeOut` sur 0,5–1,5 s canal 0 → `history.list` → `history.undo` →
  `history.redo` → `doc.saveAs` (fichier écrit) → `doc.saveAs` sur un fichier existant = `confirm_required`.
- `mcp` 1.6.0 (+ pydantic < 2.10) : 96 passed, 4 skipped (le test de progression est sauté : le
  `ClientSession` de 1.6 ne reçoit pas la progression ; le serveur, lui, l'envoie). `mcp` 1.2–1.5 ne
  s'importent plus avec les pydantic récents, d'où le plancher 1.6.
- **Non exécuté** : Linux et Windows (machine macOS, pas de conteneur). Le code propre à Linux
  (`/proc/<pid>/stat` pour la réutilisation de PID) et à Windows (`OpenProcess`, tube `\\.\pipe\…`) n'a
  tourné nulle part ; le job CI ci-dessous le couvre pour Linux.
- **Bout en bout avec l'app ouverte : pas fait** — `ControlServer`/`--control` n'existent pas encore sur
  `ai-control` (lot 1). `smoke.sh` est prêt pour ce moment-là.

## Écarts par rapport au rapport

1. **SDK épinglé `mcp>=1.6,<2`** (rapport : `mcp>=1.2`). `mcp` 2.x (actuel sur PyPI) a renommé `FastMCP` en
   `MCPServer` et refait l'API bas niveau ; l'adaptateur vise la 1.x (FastMCP + `report_progress`). Un
   portage 2.x reste à faire le jour où la 1.x ne suffira plus.
2. **Noms d'outils dynamiques `sc_<domaine>_<commande>`** (rapport : `sc.<domaine>.<commande>`) :
   l'API Anthropic n'accepte que `[a-zA-Z0-9_-]{1,64}` et Claude Code préfixe `mcp__spectrocraft__`
   (19 car.), donc ≤ 45 caractères (suffixe de hachage au-delà). `session.*`, la catégorie `debug` et le
   drapeau `Internal` sont exclus.
3. **Outils dynamiques par surcharge de `FastMCP.list_tools/call_tool`** (FastMCP ne sait pas publier un
   `inputSchema` brut) plutôt qu'un second serveur bas niveau ; ressources servies par surcharge de
   `list_resources/read_resource` (les FastMCP 1.x anciennes n'attendent pas une fonction de ressource
   `async`).
4. `confirm` n'est ajouté au schéma que si la commande n'en déclare pas déjà un, et n'est transmis que
   s'il vaut `true` (le validateur C++ tolère un `confirm` booléen de premier niveau non déclaré,
   `CommandRegistry.cpp`, `validate`).
5. `sc_save` : sans `path` → `doc.save` (en place), avec `path` → `doc.saveAs` (format réel du lot 0 :
   `doc.save` n'a pas de `path`). `sc_open` transmet `confirm` seulement si demandé.
6. Chemin Windows de l'exe : `build\src\SpectroCraft_artefacts\Release\SpectroCraft.exe` (le §7.4 omet `src\`).
7. Format réel du lot 0 différent des exemples du §6 (sans effet sur l'adaptateur, qui relaie tel quel) :
   `doc.open` renvoie le résumé du document avec `id` (rapport : `doc`) ; `history.list` renvoie `doc`
   (rapport : `docId`) et l'état 0 a `description` (rapport : `label`) ; `step.run` ne renvoie pas de
   `patchSeq` ; une erreur sur une commande `Mutates` porte quand même les champs d'état.
8. Ajouts : `eventsSinceLastCall` dans les réponses d'outils, `SPECTROCRAFT_JOB_WAIT_S` (attente max d'un
   job, défaut 600 s, ensuite « still running » sans annuler), `SPECTROCRAFT_CONTROL_DIR`,
   `SPECTROCRAFT_PIPE_BASE`, `SPECTROCRAFT_JOURNAL_DIR` (tests), `sc_client.py --events`.
9. Le rapport ne fixe pas le format de `startedAt` : l'adaptateur accepte ISO-8601, secondes ou
   millisecondes epoch.

## À faire par l'agent principal en fusionnant

### Fusion

`git merge ai-control-mcp` (aucun fichier C++/CMake/README/workflow touché ; `.gitignore` : ajout en fin
de fichier). Puis coller les deux blocs ci-dessous.

### Section README (anglais, comme le reste du README ; après « Building » par exemple)

```markdown
## AI control (Claude Code / MCP)

SpectroCraft can be driven by Claude Code (VS Code extension or terminal) while it is open: state,
history, undo/redo, any of the 20 action steps on an explicit time range and channels, AI jobs with
progress, measurements. The app runs a control server on the loopback interface only
(`127.0.0.1`, ephemeral port, per-instance token) when *Window > Allow AI control (localhost)* is
checked or when it is started with `--control`; a small Python MCP adapter in `tools/mcp` connects
Claude Code to it. `SpectroCraft --json <requests.ndjson|->` runs the same commands headless.

Setup (Python 3.11+):

    cp .mcp.json.example .mcp.json      # set SPECTROCRAFT_EXE to your executable
    # with uv: nothing else to install (uv run builds tools/mcp/.venv)
    # without uv: pip install -e tools/mcp, then use "command": "python3", "args": ["-m", "spectrocraft_mcp"]

Open the folder in VS Code / run `claude`, then ask e.g. "open recording_b.wav, select 2-3.5 s,
fade it out and show me the history". `python3 tools/mcp/sc_client.py app.state` checks the
connection without the `mcp` package. Details, Windows/Linux variants and troubleshooting:
[docs/ai-control/mcp.md](docs/ai-control/mcp.md); protocol: [docs/ai-control/RAPPORT.md](docs/ai-control/RAPPORT.md) §6.
```

### Job CI (à ajouter sous `jobs:` de `.github/workflows/build.yml`)

```yaml
  # Python MCP adapter (tools/mcp): pytest against a fake NDJSON server and a
  # fake executable. No C++ build; test_real_exe.py skips (no SPECTROCRAFT_EXE).
  mcp-tests:
    runs-on: ${{ matrix.os }}
    timeout-minutes: 10
    strategy:
      fail-fast: false
      matrix:
        os: [ubuntu-24.04, macos-15]
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: "3.11"

      - name: Install the adapter and its test dependencies
        run: pip install -e "tools/mcp[test]"

      - name: pytest
        run: pytest tools/mcp/tests -ra
```

(`windows-latest` peut être ajouté à la matrice, mais le code Windows n'a jamais tourné : à essayer sur
une branche d'abord. Les tests qui utilisent des FIFOs ou un script shell se sautent sous Windows.)

### Détails du protocole que le lot 1 doit respecter (l'adaptateur en dépend)

- **Fichier de découverte** `<userApplicationDataDirectory>/SpectroCraft/control/<pid>.json` =
  `{pid, port, token, version, startedAt, exe, protocol}`, écrit **après** que le listener accepte
  (l'adaptateur se connecte dès qu'il voit le fichier ; `sc_launch` attend le fichier dont `pid` = le
  processus lancé), supprimé à l'arrêt. `startedAt` en ISO-8601 UTC (`isoNow()`) **postérieur ou égal**
  au démarrage du processus (sinon l'adaptateur croit à un PID réutilisé : tolérance 5 s).
- **`session.hello`** : `params {token, client, subscribe:[kinds], sinceSeq?}` ; résultat = `helloInfo`
  (`protocol`, `journalSeq`…). Mauvais jeton → réponse `ok:false` `unauthorized` (fermeture permise).
  Toute commande avant le hello → `unauthorized`.
- **Événements** `{"event", "seq", "ts", "data"}` : `seq` **dans le même espace que `journalSeq`** du hello
  et que `sinceSeq`, strictement croissant. L'adaptateur initialise son dernier `seq` à `journalSeq` et
  **ignore tout événement de `seq` ≤ dernier vu** (dédoublonnage du rejeu) — sauf `events.dropped`. Si
  les événements avaient leur propre compteur, ils seraient perdus.
- **Abonnement** : `subscribe` liste des préfixes de nom d'événement (`job`, `history`, `doc`, `meta`).
- **Rejeu** : avec `sinceSeq`, l'app renvoie les événements de `seq > sinceSeq` encore en mémoire (avant ou
  après la réponse au hello, peu importe). Tant que ce n'est pas fait (lot 5), `wait_job` retombe sur
  `job.get` : `job.get` doit rester disponible pendant un job mutant (c'est le cas, catégorie `job`).
- **Jobs** : poignée `{"job":"j-N","state":"running"}` ; `job.progress {job, progress ∈ [0,1], status?}` ;
  `job.done` = `infoOf` (`ok`, `result` | `error`) — comme `JobManager.cpp` aujourd'hui.
- **Sources** : `history.changed.data.source` = `"api:" + client` (le client de l'adaptateur s'appelle
  `spectrocraft-mcp/0.1`, celui du CLI `sc_client/0.1`) ou `"menu"` ; l'adaptateur s'en sert pour ne
  signaler à Claude que les éditions des autres.
- **Lignes** : ≤ 1 Mio ; une ligne trop longue reçue par l'adaptateur coupe la connexion (il se
  reconnecte). Les réponses peuvent arriver dans le désordre (routage par `id`, ids `c1, c2…` en chaînes).
- **Tube FileForward** : garder « `int32 0` → ack `0x01`, aucun callback » (c'est la sonde de `sc_launch`
  et de `sc_status`). `--control` doit sauter `handleStartup` (§5.2).
- **Drapeau `Query`** : l'adaptateur rejoue après coupure les commandes `Query` de `app.describe` ;
  ne pas le poser sur une commande qui modifie quoi que ce soit.

## Dettes / suites

- Bout en bout avec l'app ouverte (`smoke.sh`, outils MCP réels) dès que le lot 1 est fusionné ; vérifier
  alors l'arrêt propre (`shutting_down`) et la reprise après relance de l'app.
- Portage vers `mcp` 2.x (`MCPServer`) quand nécessaire.
- Windows : exécuter la suite une fois (`OpenProcess`/`GetProcessTimes`, tube nommé, `py -3`).
- Les outils typés des lots 3-4 (`sc_noise_suppress`, `sc_transcribe`, `sc_measure_*`,
  `sc_export_spectrogram`, `sc_models`, `sc_devices`) suivent les paramètres des exemples §6.5-§6.6 ;
  quand ces commandes existeront en C++, aligner les noms de paramètres sur leurs `ParamSpec` (une
  différence donne `invalid_params` avec le `path` fautif, rien de silencieux).
