# Contrôle par IA — bilan de l'exécution du plan (lots 0 à 7)

Date : 2026-11-05/06. Branche `ai-control` du fork `nicolasmarcotteTaintin/SpectroCraft` (rien poussé
chez SuperMax3d, aucune PR). Machine : le Mac de Nicolas (macOS 26.6 arm64, AppleClang 21, Debug + IA)
dans un worktree isolé, **pas** l'agent cloud Linux prévu par la consigne (voir `baseline-macos.md`).
Linux et Windows : CI GitHub (`gh workflow run build.yml --ref ai-control`).

## État par lot

| Lot | État | Commits (`ai-control`) | Note |
|---|---|---|---|
| 0 — registre, `step.run`, journal, `--json`, correctifs | **fait** | `f1cfa62` (correctifs seuls, PR séparable), `693f3f3` | `lot-0.md` |
| 1 — serveur loopback, jobs, `UiBridge` | **fait** | `e423b9d` | `lot-1.md` (revue indépendante + correctifs) |
| 2 — adaptateur MCP Python | **fait** | fusion `80a1f4e` (`ai-control-mcp` @ `6779c5c`), `cda3390` | `lot-2.md` |
| 3 — modèles IA, devices, NS/transcription en jobs | **fait** | `502128c` (fusion `ai-control-catalog` @ `23c5d28` + commandes) | `lot-3-core.md`, `lot-3.md` |
| 4 — mesures | **fait** | fusion `2985e97` (`ai-control-measures` @ `8d4ace9`), `a217236` | `lot-4-core.md`, `lot-4.md` |
| 5 — journal persistant, Patch enrichi, rejeu | **fait** | `5679059` | `lot-5.md` |
| 6 — éclatement des commandes, migration des menus | **partiel** | `7770ce9` | `lot-6.md` (liste exacte du reste) |
| 7 — durcissement, Python, flags, stems/SR | **fait** (stems/SR non exécutés : Windows) | fusion `630dc66` (`ai-control-hardening` @ `db710c5`), `7fac071`, fusion `3a12d55` (@ `0306a32`) | `lot-7-core.md`, `lot-7.md` |

Branches parallèles fusionnées (travail d'autres agents ; leur intégration est de ma responsabilité) :
`ai-control-mcp` @ `6779c5c`, `ai-control-catalog` @ `23c5d28`, `ai-control-measures` @ `8d4ace9`,
`ai-control-hardening` @ `db710c5` puis de nouveau @ `0306a32` (fusion `3a12d55` : test Windows des
services qui lance `System32\where.exe` au lieu d'un faux `audiocpp_cli.exe`). Corrigé en les intégrant : `tools/mcp/smoke.sh` (chemin de dépôt
avec espaces), listes de sources de la cible console `SpectroCraftTests` (ModelCatalog, Sha256 en
double), conflits triviaux de `tests/UnitTests.cpp`. Le commit `502128c` est à la fois la fusion du
catalogue et la couche de commandes du lot 3.

## Tests (macOS arm64, Debug + IA, `ctest -j 8`)

| Étape | Vérifications |
|---|---|
| Référence (`76de144`) | 4859/4859 |
| Lot 0 | 5283/5284 (1 échec : tube single-instance sous charge, voir plus bas) |
| Lot 1 | 5482/5482 |
| Lot 3 | 5717/5717 |
| Lot 4 | 5998/5998 |
| Lot 5 | 6026/6026 |
| Lot 6 | 6410/6410 |
| Fusion lot 7 | 6521/6521 |
| Lot 7 (final) | **6571/6571** |

Bout en bout réel (app lancée avec `--control`, vraie fenêtre) : script NDJSON (lot 1),
`tools/mcp/smoke.sh` (lot 2), client **MCP stdio** ↔ `spectrocraft_mcp` ↔ app (lot 2) : OK.
`pytest tools/mcp/tests` : 98 passed + 4 skipped ; **102 passed** avec `SPECTROCRAFT_EXE`.
Charge : 1000 `app.state` par le socket en ~250 ms, p99 ~0,3 ms. Fuzz : 10 000 lignes NDJSON
aléatoires sans crash (après correction du crash qu'il a trouvé).

Instabilités environnementales observées : (1) plusieurs agents testaient en même temps sur ce Mac et le
dossier temporaire de la suite (`SpectroCraftTests-tmp-shard<k>`) était commun à tous les checkouts →
**corrigé** (le nom inclut le PID) ; (2) `single instance mode` (délai fixe de 2 s du tube FileForward)
échoue parfois sous charge, passe seul ; (3) après les crashs du fuzz, macOS affichait au lancement
suivant une alerte modale « rouvrir les fenêtres ? » qui bloquait la suite :
`defaults write com.SpectroCraft.SpectroCraft ApplePersistenceIgnoreState -bool YES` a été posé sur ce Mac.

## CI (workflow_dispatch sur le fork)

| Run | Tip | windows-ai | macos-ai | linux-ai | mcp-tests |
|---|---|---|---|---|---|
| 42152420659 | lot 0 `693f3f3` | ✅ | ✅ | ❌ `int64_t`→`juce::var` ambigu (corrigé au lot 1) | — |
| 42156792380 | lot 1 `e423b9d` | ✅ | ✅ | ✅ | — |
| 42158668184 | lot 4 `a217236` | ✅ | ✅ | ✅ | ✅ ubuntu + macos |
| 42160626670 | lot 6 `7770ce9` | ✅ | ✅ | ✅ | ✅ ubuntu + macos |
| 42162848694 | lot 7 `7fac071` | ⏹ annulé (bloqué dans le test des services stems/SR, boîte d'erreur système ; corrigé par `8a9cf6f`) | ✅ | ✅ | ✅ ubuntu + macos |
| 42164487280 | final `3a12d55` | ✅ | ✅ | ✅ | ✅ ubuntu + macos |

## Écarts principaux

- Machine macOS au lieu de Linux ; build et tests locaux sur macOS seulement, Linux/Windows par le CI.
- `JobManager` avancé au lot 0 (nécessaire au test de timeout de `--json`).
- Lot 6 partiel (dialogues d'effets, façades `applyActionStep`, menus File/View/Transport non migrés).
- Journal par sondage (autour des commandes, en fin de job, 30 Hz dans la fenêtre quand le serveur
  tourne) plutôt que par crochets `onChange` dispersés.
- History tab : ordre des lignes futures corrigé (bug préexistant : un clic rejouait une autre entrée
  que celle affichée).
- `tools/mcp/sc_client.py` écrit par l'agent MCP (lot 2), pas au lot 1.

## Constats à remonter à SuperMax3d

1. **Bug préexistant** (relevé par l'agent du catalogue) : les binaires audio.cpp CUDA 12.4/13.3 ne
   démarrent pas une fois installés (`installBinaryFromZip` n'extrait que `audiocpp_cli.exe`, pas
   `ggml.dll` / `ggml-base.dll`). Le catalogue les marque `installable:false`, mais les dialogues
   Stem/Super Resolution les proposent toujours.
2. **Risque produit Windows** (relevé par l'agent de durcissement) : un `audiocpp_cli.exe` corrompu ou
   tronqué peut faire lever à `CreateProcessW` une boîte d'erreur système (*hard error*) qui gèle l'app
   comme elle a gelé le runner CI. Protection proposée dans `lot-7-core.md` : `SetErrorMode(SEM_FAILCRITICALERRORS
   | SEM_NOGPFAULTERRORBOX)` autour de `CreateProcessW` dans `runProcessWithPipes`, ou vérifier l'en-tête `MZ`
   avant le lancement. Non appliqué (hors périmètre, à valider sous Windows).
3. **Bug préexistant** corrigé : History tab, lignes futures dans le mauvais ordre (lot 5).
4. Correctifs indépendants du contrôle IA, livrables en PR séparée : `f1cfa62` (envelope, device
   `auto`, `noiseSuppress` sans IA, dispatch par 1re lettre, commits côté worker de `transcribe` et
   `noiseSuppress`) ; codes de retour des `--test-*` et `--test-batch-transcribe` sur le vrai magasin
   (lot 7) ; dossier temporaire de la suite partagé entre checkouts (lot 0).
5. Migration de toute l'IA vers audio.cpp : **non faite** (demande séparée) ; l'API `ai.*` ne dépend que
   du catalogue (`engine` = donnée) et survivra à la migration.

## Questions à trancher (Nicolas / Max)

- Q10/Q13/Q17 du rapport (laissées ouvertes) : priorité réelle des stems/SR ; fork ou contribution
  amont (les lots 6-7 touchent `MainWindow.cpp`, coûteux à rebaser) ; Python 3.11+ et `mcp>=1.6,<2`
  installables sur les postes verrouillés ?
- Lot 6 : les dialogues d'effets doivent-ils appeler le registre (formulaire → `runMenuCommand`) et le
  batch d'actions passer par `--json` (façades) ?
- Périmètre de chemins : ajouter d'emblée les partages réseau des programmes (Q18) ?
- `killTreeOnExit` vrai par défaut aussi pour la GUI (lot 7 cœur) : un script qui lance volontairement
  un processus de fond le verra tué.
- Protocole `1` encore « expérimental » : quand le figer (Q11) ?

## Reste à faire

- Lot 6 : voir `lot-6.md` (façades, 19 sites `recordActionStep` des dialogues, menus File/View/
  Transport/Selection/Record, `export.snapshot`, `drawing.*`, `clone.*`, `selection.saved.*`,
  `actions.batch`).
- `export.spectrogram` avec overlays (`UiBridge` → `preparePngExport`).
- Stems/SR à exécuter sur un poste Windows avec les binaires et modèles audio.cpp.
- Windows : résolution des liens au milieu d'un chemin (périmètre), ACL du fichier de découverte.
- `step.run {type:"python"}` sans timeout (préférer `python.run`).
- Mesure de dérive mémoire sur 10 000 commandes.
