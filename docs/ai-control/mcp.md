# Adaptateur MCP SpectroCraft (Claude Code)

`tools/mcp` contient le serveur MCP qui permet à Claude Code (VS Code ou terminal) de piloter
SpectroCraft **ouvert** : état, historique, undo/redo, étapes d'édition sur une plage précise, jobs IA,
mesures. Architecture et protocole : `RAPPORT.md` §5-§7. Consignes d'usage pour Claude : `CLAUDE.md`
à la racine.

```
Claude Code ──stdio MCP──▶ spectrocraft-mcp (Python) ──TCP 127.0.0.1:<port>, NDJSON──▶ SpectroCraft
                             lit <appData>/SpectroCraft/control/<pid>.json {port, token}
```

## Prérequis

- **Python 3.11+** et le SDK `mcp` **1.x** (`mcp>=1.6,<2` ; la série 2.x a renommé FastMCP et n'est pas
  encore prise en charge). Rien d'autre.
- Une version de SpectroCraft qui accepte `--control` / *Window > Allow AI control (localhost)*
  (serveur de contrôle, lot 1). Le mode sans fenêtre `--json` (outil `sc_headless`) existe dès le lot 0.
- Aucune règle de pare-feu : tout passe par la boucle locale `127.0.0.1` sur un port éphémère ; rien
  n'écoute sur le réseau.

## Installation

### Variante `uv` (recommandée)

`uv` crée et met à jour l'environnement tout seul (`tools/mcp/.venv`, ignoré par git).

```sh
# macOS / Linux
brew install uv            # ou : curl -LsSf https://astral.sh/uv/install.sh | sh
cp .mcp.json.example .mcp.json
# éditer SPECTROCRAFT_EXE dans .mcp.json, puis ouvrir le dossier dans VS Code / lancer `claude`
```

`.mcp.json` (local, ignoré par git) — valeurs macOS du gabarit :

```json
{"mcpServers":{"spectrocraft":{
  "command":"uv","args":["run","--directory","tools/mcp","spectrocraft-mcp"],
  "env":{"SPECTROCRAFT_EXE":"/Applications/SpectroCraft.app/Contents/MacOS/SpectroCraft",
         "SPECTROCRAFT_LAUNCH":"ask"}}}}
```

### Variante `pip`

```sh
python3 -m venv .venv && . .venv/bin/activate      # Windows : py -3 -m venv .venv ; .venv\Scripts\activate
pip install -e tools/mcp                           # + "[test]" pour lancer les tests
```

puis, dans `.mcp.json`, remplacer `command`/`args` par l'interpréteur de cet environnement :

```json
"command": "/chemin/du/depot/.venv/bin/python", "args": ["-m", "spectrocraft_mcp"]
```

(ou `"command":"spectrocraft-mcp"` si le script installé est dans le `PATH` de Claude Code).

### Windows

```json
{"mcpServers":{"spectrocraft":{
  "command":"py","args":["-3","-m","spectrocraft_mcp"],
  "env":{"SPECTROCRAFT_EXE":"C:\\chemin\\du\\depot\\build\\src\\SpectroCraft_artefacts\\Release\\SpectroCraft.exe",
         "SPECTROCRAFT_LAUNCH":"ask"}}}}
```

après `py -3 -m pip install -e tools/mcp`. La variante `uv` marche aussi telle quelle (`uv` pour
Windows). Le chemin réel de l'exe est `build\src\SpectroCraft_artefacts\<Config>\SpectroCraft.exe`
(le rapport §7.4 omettait `src\`).

### Linux

Identique à macOS ; `SPECTROCRAFT_EXE` pointe vers `build/src/SpectroCraft_artefacts/<Config>/SpectroCraft`.
Sans session graphique, seul `sc_headless` (mode `--json`) fonctionne.

### Vérifier

```sh
python3 tools/mcp/sc_client.py --list        # instances détectées (aucune dépendance)
python3 tools/mcp/sc_client.py app.state     # état de l'app ouverte
```

Dans Claude Code : `/mcp` doit montrer `spectrocraft` connecté ; demander « sc_status ».

## Variables d'environnement

| Variable | Rôle |
|---|---|
| `SPECTROCRAFT_EXE` | exécutable (ou bundle `.app`) pour `sc_launch` et `sc_headless` |
| `SPECTROCRAFT_LAUNCH` | `ask` (défaut : erreur claire qui propose `sc_launch`), `auto` (lance l'app au premier outil), `never` (jamais : utiliser `sc_headless`) |
| `SPECTROCRAFT_PID` | instance à piloter quand plusieurs sont ouvertes (sinon la plus récente) |
| `SPECTROCRAFT_JOB_WAIT_S` | attente maximale d'un job par un outil (défaut 600 s ; au-delà l'outil rend la main, le job continue) |
| `SPECTROCRAFT_CONTROL_DIR` | dossier des fichiers de découverte (tests ; défaut ci-dessous) |
| `SPECTROCRAFT_MCP_LOG` | niveau de log sur stderr (`INFO`, `DEBUG`) |

Fichiers de découverte `<pid>.json` : macOS `~/Library/SpectroCraft/control`, Linux
`$XDG_CONFIG_HOME/SpectroCraft/control` (défaut `~/.config/…`), Windows `%APPDATA%\SpectroCraft\control`.

## Outils

Outils fixes (§7.3) : `sc_status`, `sc_launch`, `sc_state`, `sc_history`, `sc_undo`, `sc_redo`,
`sc_jump`, `sc_run_step`, `sc_run`, `sc_open`, `sc_save`, `sc_close`, `sc_models`, `sc_install_model`,
`sc_devices`, `sc_noise_suppress`, `sc_transcribe`, `sc_measure_levels`, `sc_third_octave`,
`sc_spectrum`, `sc_export_spectrogram`, `sc_jobs`, `sc_cancel`, `sc_headless`.

Outils **dynamiques** : un par commande publiée par `app.describe` (hors `session.*` et commandes
internes), nommés `sc_<domaine>_<commande>` (`sc_selection_set`, `sc_step_run`…). Leur `inputSchema` est
le schéma `params` de la commande, inchangé, plus trois propriétés optionnelles ajoutées par
l'adaptateur : `doc`, `timeoutMs`, `confirm`. Ils apparaissent au premier contact avec l'app (notification
`tools/list_changed`) : une commande ajoutée côté C++ est disponible sans toucher au Python.

Ressources : `spectrocraft://commands` (describe complet) et `spectrocraft://journal/tail` (fin du
journal de session, JSON Lines).

## Dépannage

| Symptôme | Cause / solution |
|---|---|
| `control_disabled` : « SpectroCraft is open but its AI control server is off » | l'app est ouverte (le tube `SpectroCraft.FileForward` répond) mais le contrôle est coupé : *Window > Allow AI control (localhost)*, ou relancer l'app avec `--control`, ou `sc_launch(force=true)` (seconde instance) |
| `no_instance` | aucune app : `sc_launch` (avec `SPECTROCRAFT_EXE`), `SPECTROCRAFT_LAUNCH=auto`, ou `sc_headless` |
| `launch_failed` « did not write its discovery file » | version de l'app antérieure au serveur de contrôle (lot 1), ou crash au démarrage : lancer `SpectroCraft --control` à la main |
| `unauthorized` | jeton périmé (app relancée) : `sc_status` relit les fichiers ; trois échecs → pause de 2 s côté app |
| `timeout` | une boîte de dialogue modale bloque le thread de l'app : la fermer |
| `busy` | un job modifiant ou un dialogue de progression est actif : `sc_jobs`, attendre ou `sc_cancel` |
| `protocol_mismatch` | app et adaptateur de versions de protocole différentes : mettre à jour l'un des deux |
| Pas d'outils `sc_<domaine>_…` | normal tant qu'aucune app contrôlable n'a été contactée ; appeler `sc_status` |
| `ModuleNotFoundError: mcp.server.fastmcp` | `mcp` 2.x installé à la main : `pip install "mcp<2"` (le `pyproject` l'impose déjà) |
| Pare-feu | aucun réglage nécessaire : loopback uniquement, port éphémère |

Fichiers `<pid>.json` orphelins (crash) : supprimés automatiquement par l'adaptateur (PID mort, ou PID
réutilisé par un processus plus récent que `startedAt`).

## Tests

```sh
pip install -e "tools/mcp[test]"
pytest tools/mcp/tests                                   # faux serveur NDJSON + faux exe, ~10 s
SPECTROCRAFT_EXE=<exe> pytest tools/mcp/tests/test_real_exe.py   # vrai mode --json
SPECTROCRAFT_EXE=<exe> tools/mcp/smoke.sh /abs/fichier.wav       # app ouverte (--control), local
```
