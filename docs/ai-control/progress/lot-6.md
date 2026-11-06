# Lot 6 — éclatement des commandes et migration des menus

Statut : **partiel, solide** (ce qui est livré est testé ; la liste exacte de ce qui reste est en bas).

## Fait

### Commandes typées (`src/control/commands/Edit.cpp`)

- Les **20 types d'étapes** ont une commande typée dont l'alias est l'ancien nom de type :
  `effect.silence/fadeIn/fadeOut/normalize/reverse/invert/changeGain {db}/timeStretch {pct}/
  catTimeScale/filterEq {points}/envelope {points}/resample {rate, weights}`, `gen.silence/noise/tone`,
  `edit.delete/trim`, plus `ai.noiseSuppress`/`ai.transcribe` (lot 3) ; `python` → `python.run` au lot 7.
  Chaque commande a ses `ParamSpec` (schéma publié, `invalid_params` avec `path`) + `select`/`channels`
  et passe par le **même** exécuteur que `step.run` (jobs, enregistrement d'action, journal).
- `edit.cut/copy/paste/pasteMix/pasteToNewChannels/copyToNewFile {select}`, `edit.splitSpectralSel`.
- `doc.new {channels, sampleRate, durationSec}` (`doc.activate` existait depuis le lot 0).
- `selection.tool`, `selection.clearSpectral`, `selection.brush`.
- `settings.get {keys}` / `settings.set {values}` : table de clés (mêmes noms que `SpectroCraft.recent`)
  avec **les mêmes bornes** que `loadSettings` (`fft_size` 99999 → 65536, `clamped:["fft_size"]`),
  validation complète avant application, persistance (`saveSettings` + `saveAiSettings`). **Sécurité** :
  `control_enabled`/`control_roots` ne sont pas modifiables par l'API (un client ne peut pas élargir
  son propre accès) ; clé inconnue → `invalid_params`.
- `recent.list/remove/clear`, `meta.setMode {embedded|external}`, `meta.save`,
  `record.start/stop/setMode`, `actions.list`, `actions.run {name|index, select, channels}` (une entrée
  d'historique par étape), `tables.list`, `tables.importCsv`.
- `docs/commands.md` **généré** (`SpectroCraft --list-commands --markdown`, trié par domaine) :
  100 commandes.

### Menus migrés (passent par `CommandRegistry::invoke`, source `menu`)

`MainWindow::runMenuCommand (cmd, params, effectId, quiet)` : même code, même rafraîchissement
(`UiBridge`), même enregistrement d'action, même journal et même provenance de patch (`opId` =
commande) que l'API ; un fichier streamé passe par le détour du registre (job + dialogue de
progression, erreur affichée à la fin du job), une erreur synchrone par `showEditError`.

| MenuID | Commande |
|---|---|
| `EditUndo`, `EditRedo` | `history.undo`, `history.redo` |
| `EditCut`, `EditCopy`, `EditPaste`, `EditPasteMix`, `EditPasteToNewChannels`, `EditCopyToNewFile` | `edit.*` |
| `EditDelete`, `EditTrimSelection` | `edit.delete`, `edit.trim` |
| `EditSelectAll` | `selection.all` |
| `ToolsSilence`, `ToolsFadeIn`, `ToolsFadeOut`, `ToolsNormalize`, `ToolsReverse`, `ToolsInvert` | `effect.*` (l'id « répéter le dernier effet » est conservé) |

Soit **17 MenuIDs** et **8 des 27 sites `recordActionStep`** (ceux des effets immédiats et de
Delete/Trim) : l'enregistrement d'action se fait maintenant dans le registre, après succès.

- `continueReplay` : nouveau rappel `MainWindow::onReplayFinished (ok, message)` qui remplace les boîtes
  modales « finished / stopped » quand il est posé (l'API `actions.run` n'utilise de toute façon pas
  `continueReplay`).

## Tests (`tests/regression/menu_commands.inc`, « regression: menu_commands »)

- **menu == commande** pour les 8 menus d'effets/édition : le menu (`menuItemSelected` sur une vraie
  `MainWindow`) et l'API sur le même fichier et la même plage donnent **des échantillons identiques** ;
  le patch du menu porte `opId` = la commande et `source:"menu"` ; `getLastEffectId` conservé.
- Couper/coller/annuler/tout sélectionner par le menu passent par le registre (`edit.cut` crédité).
- Chaque type d'étape a sa commande typée ; `effect.changeGain` == `step.run changeGain` ; bornes
  validées ; alias ; `gen.tone` ; presse-papiers ; `edit.copyToNewFile` ; `doc.new` ;
  `selection.tool/brush` ; `settings.set` (borne, refus `control_*`, clé inconnue) / `settings.get` ;
  `recent.list` ; `meta.setMode/save` ; `actions.run` (2 entrées d'historique) / `actions.list` ;
  `tables.list` ; `record.stop/setMode`.

Résultats (macOS arm64, Debug + IA) : « menu_commands + control » 1111/1111 ; suite complète
**6410/6410**, 10/10.

## Ce qui reste (précisément)

1. **Façades** : `applyActionStep`, `validateStepParams`, `isKnownActionType` restent les
   implémentations d'origine (le registre les appelle) au lieu de devenir des façades de 5 lignes sur
   `invoke` : le rejeu d'actions enregistrées et le batch (`runActionBatch`, `--run-action`, `--batch`)
   tournent sur des `Application` isolées sur des workers, où `invoke` (thread de messages) ne
   s'applique pas tel quel. À faire avec Max : décider si le batch passe par `--json`.
2. **Dialogues d'effets** (19 sites `recordActionStep` restants) : `showChangeGainDialog`,
   `TimeStretch`, `CatTimeScale`, `FilterEq`, `Envelope`, `Resample`, `GenSilence/Noise/Tone`,
   `NoiseSuppress`, `Transcribe`, Python : le dialogue reste le formulaire et applique lui-même ; il
   faudrait que son bouton « Apply » appelle `runMenuCommand ("effect.changeGain", {db})` etc. Les
   éditions qu'ils produisent sont quand même journalisées et attribuées (`opId` = type d'étape,
   `source:"menu"`, lot 5).
3. **MenuIDs non migrés** (ont une commande API mais le menu garde son chemin propre) : `FileOpen/Save/
   SaveAs/Close/CloseAll/New/Quit` (FileChooser / confirmations), `View*` (zoom, vues, grilles :
   bascules de `settings`), `Transport*` (le menu garde `playWithAudioInit` et son dialogue),
   `Selection*`, `Rec*`, `Meta*`, `Exp*`, `WindowAllowMultiple`, `Recent*`, `ActionsRunSelected/Batch`,
   `ToolsSplitSpectralSel*`.
4. **Sans commande** : `export.snapshot` (`FileSnapshot`, NeedsUi), `drawing.*`, `clone.*`,
   `selection.saved.*`, `actions.batch`, `export.spectrogram` avec overlays (lot 4), `tables.*` autres
   que list/importCsv.
5. Test « menu == commande » étendu aux commandes ci-dessus quand elles seront migrées.
