# Lot 5 — journal persistant, Patch enrichi, événements avec rejeu

Statut : **fait**.

## Ce qui a été fait

- **`Patch` enrichi** (`include/UndoPatch.h`) : `seq` (monotone par document), `timestampMs`, `opId`,
  `params` (JSON, métadonnées), `source` (`menu`, `api:<client>`, `cli`, `test`). Posés par
  `UndoManager::record` (le seul point d'enregistrement, §3 #7) depuis un **contexte d'opération**
  (`sc::undoctx`, `include/UndoManager.h`) :
  - une portée sur le thread courant (`undoctx::Scoped`) : posée par `CommandRegistry::invoke` autour
    du handler (`opId` = id de la commande, `params` = ses paramètres sans jeton, `source` = client)
    et par `ScopedInvokeSource` autour des commits de jobs ;
  - sinon une opération « en attente » annoncée par `Application::recordActionStep` (dialogues/menus :
    type d'étape + params, source `menu`) et prise par le prochain `record()` **sur n'importe quel
    thread** (l'édition peut tourner sur un worker), valable 60 s ; les jobs IA/Python l'utilisent aussi
    (leur patch est enregistré dans le hop de commit de l'`Application`, hors de toute portée) ;
  - sinon `opId:"menu"`, `source:"menu"` (couper/coller, souris…).
  La pile en attente ne garde que des chaînes (un `static` ne doit pas posséder de `juce::var` : le
  détecteur de fuites de JUCE s'en plaignait).
- **Journal complet** (`ControlState::pollDocuments`) : à chaque commande (avant/après), à chaque fin de
  job, et à 30 Hz par le minuteur de la fenêtre **quand le serveur de contrôle tourne** :
  - `history` `recorded` par nouveau patch (`patchSeq, ts, opId, params, source, description,
    undoDepth, redoDepth`) — y compris les éditions à la souris ; `undo`/`redo` (déjà journalisés par
    `undoSteps/redoSteps`) ; `cleared` avec raison (`undo failed`, `redo failed`, `resample` déduit d'un
    changement de fréquence, `unknown`) ;
  - `meta` `changed` quand le compteur `metaDirty` d'un document bouge (marqueurs, sélections, dessins,
    tables, transcription : on donne les comptes, jamais le contenu) ;
  - `doc` `opened` / `closed`.
  Les entrées `command` ne portent plus `undoDepth/description` : c'est l'entrée `history recorded`
  qui attribue chaque patch.
- **Persistance JSON Lines** : `<userApplicationDataDirectory>/SpectroCraft/journal/
  session-<pid>-<yyyy-mm-dd>.jsonl` (macOS `~/Library/SpectroCraft/journal`), activée au démarrage du
  serveur de contrôle (Q6), rotation au-delà de 64 Mo (`.1.jsonl`), 30 sessions gardées.
  `journal.read {sinceSeq}` relit le fichier pour les entrées sorties de l'anneau (10 000) et recoud
  sans trou avec l'anneau ; `file` indique le chemin.
- **Événements** : une entrée = un événement (`history.changed`, `meta.changed`, `doc.changed`, sinon
  `journal.entry` si abonné), `seq` = celui du journal ; les événements non journalisés (`job.*`,
  `doc.changed` des commandes) prennent un `seq` réservé (`Journal::reserveSeq`) dans le même compteur.
  Rejeu `sinceSeq` à `session.hello` / `events.subscribe` (ordre croissant).
- **`history.list`** lit `patchSeq/ts/opId/params/source` dans les patches (la jointure best-effort du
  lot 0 ne sert plus que de repli) ; enveloppe des commandes `Mutates` : **`patchSeq`** du dernier patch.
- **History tab** : les éditions faites par l'API affichent à droite `hh:mm:ss  api:<client>` (rien ne
  change pour les éditions faites dans la fenêtre, pour ne pas toucher l'aspect ni les tests de rendu) ;
  **correctif** : les lignes futures sont listées dans l'ordre de redo (le constat du lot 0 : un clic
  sur la première ligne future rejouait une autre entrée que celle affichée).

## Tests (`tests/regression/journal.inc`, « regression: journal »)

Journal autonome : anneau de 10 000, miroir JSONL de chaque entrée, lecture au-delà de l'anneau depuis
le fichier puis couture sans trou, `reserveSeq`. Patches : édition API (seq, heure, commande, params,
client, `patchSeq` dans l'enveloppe), édition de dialogue (type d'étape + params + `menu`), édition
sans type (`menu`). Journal : une entrée `recorded` par patch, attributions API/menu, idempotence du
sondage, undo, marqueur → `meta`, rééchantillonnage → `cleared {reason:"resample"}`, ouverture/fermeture.
`history.list` : chaque état porte `patchSeq/ts/opId/source` ; futurs dans l'ordre de redo ; History
tab dans le même ordre, détail heure+client pour l'API, et la 1re ligne future est bien celle que
`redoSteps(1)` rejoue. Serveur (`control_server.inc`) : un nouveau client avec `sinceSeq:0` reçoit les
`history.changed` rejoués dans l'ordre des `seq`.

Résultats : voir en bas.

## Écarts / dettes

- Pas de crochet `MarkerManager::onChange` ni de `UndoManager::onChanged` : un **sondage** (à chaque
  commande, fin de job, et 30 Hz dans la fenêtre si le serveur tourne) couvre tous les chemins sans
  câbler chaque création de `FileDoc` (une douzaine de sites) ; latence ≤ 33 ms dans l'app ouverte.
  Le journal d'une session headless (`--json`) n'est complet qu'aux frontières de commandes, ce qui y est
  équivalent.
- Une annonce `recordActionStep` suivie d'une annulation du dialogue reste « en attente » ≤ 60 s et
  peut étiqueter la prochaine édition à la souris.
- La profondeur d'undo reste 64 ; les patches éjectés gardent leur entrée `recorded` dans le journal.

## Résultats

macOS arm64, Debug + IA : « journal,control,history » 907/907 ; suite complète **6026/6026**, 10/10.
