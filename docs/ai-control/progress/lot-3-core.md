# Lot 3 — cœur « modèles IA, devices » (sans registre de commandes)

Branche `ai-control-catalog`, créée depuis `ai-control` = `76de144` (le commit de docs `5a80afd`
indiqué dans la consigne a été amendé en `76de144` sur le fork ; seule différence : deux lignes de
`DECISIONS.md`). Ne dépend ni de `src/control/` ni du `JobManager` : le lot 0 du fil principal
(`693f3f3`) n'est pas inclus, les wrappers `ai.*` restent à écrire (voir « Fusion » plus bas).

## Fait

1. **`sc::ai::ModelCatalog`** (`include/AI/ModelCatalog.h`, `src/AI/ModelCatalog.cpp`) : une vue
   unique sur les deux registres. 32 entrées : 15 de `ModelManager` (3 OpenVINO + 12 Whisper),
   4 binaires audio.cpp (`audiocpp-cli-<variant>`), 9 SAM-Audio, FlashSR, AudioSR, 2 htdemucs
   « manuels » (détectés seulement, pas de source de téléchargement dans l'app).
   - Champs : `id, engine (openvino|whisper|audiocpp), family (noiseSuppress|transcribe|stems|superRes|runtime),
     displayName, folder, files[{name,url,sizeBytes,sha256}], archive (zip des runtimes), variant,
     backends, platforms, license, source, installable, reason, reasonCode, manual`.
   - Moteur = donnée : la migration vers l'API C d'audio.cpp (demande de SuperMax3d, **non faite ici**)
     ajoutera des entrées / un nouvel `engine`, sans toucher au format JSON.
   - `status()` (stat seulement, aucun hash), `installPlan()` (pré-contrôle synchrone pour le handler),
     `install()` / `verify()` / `remove()` **bloquants**, sûrs depuis un worker, progression + annulation
     par `CatalogProgressFn (float, status) -> bool`, garde « busy » par id (les 4 variantes du CLI
     partagent une seule clé : un seul `audiocpp_cli.exe`).
   - Codes d'erreur = sous-ensemble de `sc::control::err` : `invalid_params`, `busy`, `cancelled`,
     `unsupported_platform`, `model_not_installed`, `failed` → un wrapper transmet `CatalogOutcome` tel quel.
   - JSON §6.5 : `listJson()` (`{"modelsRoot","audiocppRoot","models":[…]}`), `entryJson()`,
     `removalPreview()` (texte pour `confirm_required` : « Removes 148 MB under … »), résultats
     d'install/verify/remove aux formes de `job.done` du §6.5.
2. **SHA-256 des assets audio.cpp** : les 15 assets référencés ont été **téléchargés et hachés**
   (2026-11-05) ; chaque valeur est égale au `digest` publié par GitHub (releases) ou à l'oid LFS
   publié par Hugging Face. Ajoutés dans `binaryChoices/modelChoices/flashSrChoices/audioSRChoices`
   (`include/AudioCpp.h` : champs `sha256`, `exeSha256`, `exeSizeBytes`, `catalogBlocker`).
   - `sc::acpp::downloadFile` vérifie maintenant le SHA-256 en flux (paramètre optionnel, sinon
     `knownSha256ForUrl(url)`) : les dialogues Stem/Super Resolution, inchangés, en profitent.
     `commitDownload` refuse un hash faux (le `.part` est supprimé, la destination intacte).
   - `installBinaryFromZip` extrait dans un dossier temporaire, compare `audiocpp_cli.exe` au hash
     publié de la variante, puis seulement remplace le binaire en place (un zip refusé ne détruit plus
     le binaire existant).
   - URL FlashSR épinglée sur le commit `38156353` au lieu de `main` (sinon le hash publié pourrait
     cesser de correspondre).
   - Téléchargement/exécution audio.cpp toujours **Windows seulement** (stubs POSIX inchangés).
3. **Devices réels** : `sc::ai::resolveAiDevice(engine, requested, available)` (pur) +
   `resolveAiDevice(engine, requested)` / `resolveAiDevice(requested)` (listes réelles),
   `availableDevices()`, `audioCppBackendsForVariant()`, `devicesJson()` (forme §6.5 ; sans `init`,
   OpenVINO n'est pas chargé : `"initialised":false,"devices":null`). Un device indisponible est une
   **erreur explicite** (`invalid_params`, « device GPU is not available for openvino »,
   `details.expected = "one of [auto, CPU]"`), jamais un repli silencieux. L'ancien
   `sc::resolveAiDevice` (repli CPU + note) reste pour les dialogues, non modifiés.
4. **`SC_MODELS_DIR` sur toutes les plateformes** : `ModelManager` (racine), `sc::acpp::dir()`
   (`<SC_MODELS_DIR>/audiocpp`, avant : jamais), et `SC_OPENVINO_CACHE_DIR` aussi sous Windows.
5. **Installs `file://`** dans `ModelManager` (`localFileGet` : mêmes contrôles taille + SHA-256,
   `.part`, annulation) : miroir hors-ligne et fixtures de tests sans réseau.
6. **`buildNsFailureDetails`** déplacé de `src/MainWindow.cpp` vers `src/NoiseSuppression.cpp`
   (module IA ; la déclaration était déjà dans `include/NoiseSuppression.h`). MainWindow : suppression seule.
7. `src/AI/Sha256.cpp` passe dans les sources toujours compilées (AudioCpp.cpp en a besoin même hors IA).

## Table des hashes (assets audio.cpp)

| Asset | SHA-256 | Taille (octets) | Source |
|---|---|---|---|
| bin cpu-portable (zip) | `9654c8f66f87bda768196d934059be896399b43703b06e5f2b857430775bec3b` | 28786884 | https://github.com/0xShug0/audio.cpp/releases/download/v0.9.2-drama3-base-small/audio-v0.9.2-drama3-base-small-bin-windows-x64-cpu-portable.zip |
| ↳ `audiocpp_cli.exe` | `427875df84983e950103caa78c435e2dd3cef4b99dc0295c4f941e49598fda06` | 27326464 | (dans le zip) |
| bin cuda12.4 (zip) | `48852da40790c40f3285bca6c53ff7829b3963080f0e95b6742f868fc1d7c757` | 464268606 | …/audio-v0.9.2-drama3-base-small-bin-windows-x64-cuda12.4.zip |
| ↳ `audiocpp_cli.exe` | `ee1ae0b3dce0918f33dd9446e55c21f50120bcf4501a98a9a9a1ef4b7eaa91a9` | 26529280 | (dans le zip) |
| bin cuda13.3 (zip) | `b570efa4b17c2aa32c431c653489c009b58f57f60d68782b3b94b6c06304a9e8` | 275837338 | …/audio-v0.9.2-drama3-base-small-bin-windows-x64-cuda13.3.zip |
| ↳ `audiocpp_cli.exe` | `baf36544d8734099b5fb22c3284f5a0c4d5dba0272f36e8180a16d77bda2e7c9` | 26537472 | (dans le zip) |
| bin vulkan-portable (zip) | `9c943d406af3543cc5005d158ed583210cd7a13371bffcd4b7f262f7a6a5ad16` | 63227361 | …/audio-v0.9.2-drama3-base-small-bin-windows-x64-vulkan-portable.zip |
| ↳ `audiocpp_cli.exe` | `373e76168f317f7b8f63bf0ccdf1f2048c01cc995fdcdd507a17264d3d4e09a4` | 84764672 | (dans le zip) |
| flashsr/flashsr.safetensors | `cfa15eb8576e50cff30bc9851fc2ca8e74b6ea379fbcded03c9cc44458b1c6e2` | 356008 | https://github.com/0xShug0/audio.cpp/raw/38156353396fae68d10388c960b900a8fda8373f/assets/framework/audio_utilities/flashsr/flashsr.safetensors |
| sam-audio-small-q8_0.gguf | `82e7aaf7b8c458f67183a4924950dbb10ad1979a9c057f1a906c79752a4ba243` | 2379290272 | https://huggingface.co/audio-cpp/SAM-Audio-GGUF/resolve/main/sam-audio-small-q8_0.gguf |
| sam-audio-small-bf16.gguf | `48e51ad99e08eb976cbcbf65e1dd725b0e76b00618fd857a05dc7b7dadceb46b` | 2824105568 | …/SAM-Audio-GGUF/resolve/main/sam-audio-small-bf16.gguf |
| sam-audio-small-f32.gguf | `ec80610e076c7408dcc14ba8c7bc6c321f8d691bcf093e94882898a9dbaf34f1` | 5640890208 | …/SAM-Audio-GGUF/resolve/main/sam-audio-small-f32.gguf |
| sam-audio-base-q8_0.gguf | `e5529e940218981b67be2c58904ffca7b7eecba20b65e2e99e7d23eb5eb7bd9b` | 3095951072 | …/SAM-Audio-GGUF/resolve/main/sam-audio-base-q8_0.gguf |
| sam-audio-base-bf16.gguf | `90f658f0043859e195ef08a298dd61d1747bc65245c0768f2d0863f3e42849b2` | 4136791712 | …/SAM-Audio-GGUF/resolve/main/sam-audio-base-bf16.gguf |
| sam-audio-base-f32.gguf | `34b1fec64c1e07027e7b30feb744f59cd00bc10a02daa8f86140f0fe054242cc` | 8265738720 | …/SAM-Audio-GGUF/resolve/main/sam-audio-base-f32.gguf |
| sam-audio-large-q8_0.gguf | `2ff2d4599da19353105156582f995aa433899450c27e0274f0e45cc51369abf3` | 5029984064 | …/SAM-Audio-GGUF/resolve/main/sam-audio-large-q8_0.gguf |
| sam-audio-large-bf16.gguf | `d3582311e5a43ae1e067b3a36c2c85a5dea946757114c611892a0fdb3e67b3ee` | 7705276160 | …/SAM-Audio-GGUF/resolve/main/sam-audio-large-bf16.gguf |
| sam-audio-large-f32.gguf | `bdfe2614aca0a85eebf2304959f9783125655ecb9475e56cd180f4551d32043a` | 15401675328 | …/SAM-Audio-GGUF/resolve/main/sam-audio-large-f32.gguf |
| AudioSR-GGUF/audiosr-basic-f32.gguf | `bf92d26c638e82f5051dfd8cf5899798d64ce5366ff63bad8b5930a82fce5bff` | 6177855616 | https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/main/AudioSR-GGUF/audiosr-basic-f32.gguf |

Tous les fichiers : HTTP 200, taille reçue = taille du catalogue. Les tailles étaient déjà exactes.
Aucun asset n'est resté sans hash.

## État `installable` (catalogue intégré)

- OpenVINO / Whisper (15) : `installable:true` partout (hashes déjà présents).
- audio.cpp, Windows : CPU portable, Vulkan portable, 9 SAM-Audio, FlashSR, AudioSR → `true`.
- audio.cpp hors Windows : `false`, `reasonCode:"unsupported_platform"`.
- **CUDA 12.4 / 13.3 : `false`, `reasonCode:"blocked"`** — constat : leur `audiocpp_cli.exe` importe
  `ggml.dll` et `ggml-base.dll` (fournis dans le même zip), alors que `installBinaryFromZip` n'extrait
  que l'exe ; un binaire CUDA installé ainsi ne peut pas démarrer (non exécuté : déduit de la table
  d'imports). Les variantes « portable » n'importent que le runtime MSVC. Les dialogues proposent
  toujours CUDA (StemSeparation.cpp hors de mon périmètre) : à corriger en extrayant toutes les DLL.
- htdemucs-6stems / htdemucs-ft : `false`, `reasonCode:"manual"` (les fichiers existent sur
  `audio-cpp/audio.cpp-gguf/HTDemucs-6stems-GGUF`, mais l'app n'a pas d'URL ; non ajoutée).

## Tests

`tests/regression/ai_catalog.inc` (suite `regression: ai_catalog`, enregistrée dans `tests/UnitTests.cpp`,
2 lignes) — aucun réseau :
- hashes acpp (tous présents, recherche par URL, FlashSR épinglé), `validateSha256`, `commitDownload`
  avec hash faux/juste, `sha256OfFile`, `installBinaryFromZip` (exe refusé, binaire existant préservé,
  variante inconnue acceptée) ;
- `SC_MODELS_DIR` (ModelManager + `acpp::dir()`, valeur relative ignorée) — tourne aussi sous Windows ;
- catalogue intégré : 32 entrées, ids uniques, champs cohérents, `installable ⇒ sha256 + taille + URL`,
  raisons présentes, CUDA bloqué, plateforme ;
- JSON §6.5 après aller-retour texte (`whisper-base`, tailles > 4 Go, runtime), `devicesJson` ;
- `resolveAiDevice` : auto/GPU/NPU/CPU, `GPU.n`, casse, refus explicites, moteur inconnu, whisper, audio.cpp ;
- install/verify/remove par `file://` (annulation sans fichier partiel, progression, corruption
  détectée puis réparée, hash publié faux refusé, entrée non installable, busy concurrent par thread,
  aperçu de suppression, suppression idempotente, id inconnu) ;
- chemin d'install audio.cpp (zip + exe + variante, modèle en sous-dossier, hash faux refusé, slot CLI
  partagé busy) via un téléchargeur injecté (`setAudioCppDownloaderForTests`), donc testé sur macOS/Linux.

Résultats (macOS 26 arm64, AppleClang 21, Debug, `SPECTRO_ENABLE_AI=ON`, ctest 8 shards) :
- base `76de144` : 10/10 ctest OK ;
- après le lot : 10/10 ctest OK (deux passes complètes, avant et après les corrections de revue) ;
  `regression: ai_catalog` ≈ 0,04 s.
- build `SPECTRO_ENABLE_AI=OFF` (macOS) : compile ; `regression: ai*` 43/43 (les parties catalogue
  sont sautées avec `SKIP`, les contrôles SHA-256 audio.cpp et `SC_MODELS_DIR` tournent).
- interférences locales, sans rapport avec le lot : d'autres agents lançaient la suite en même temps
  sur la même machine. (a) `scTestTempDir()` = `<temp>/SpectroCraftTests-tmp-shard<k>` est global et
  vidé au démarrage de chaque passe → une passe a eu des dizaines d'échecs « write wav » + SegFaults ;
  avec un `TMPDIR` privé, disparus. (b) `single instance mode` (tube nommé global) échoue parfois
  (« empty list acknowledged », « second client's forward acknowledged ») ; seul : 13/13. Dernière
  passe complète (TMPDIR privé) : 9/10 shards OK, le 10e uniquement sur ce test de tube.
  Le CI (machines isolées) fait foi.
- **CI** `build.yml` (workflow_dispatch sur `f12f175`, run 42155383758) : `windows-ai` ✅ (MSVC Release,
  chemins audio.cpp Windows compilés et testés), `macos-ai` ✅, `linux-ai` ✅.

## Écarts

- Base `76de144` et non `5a80afd` (voir en tête).
- Fichiers hors liste touchés : aucun, sauf `tests/UnitTests.cpp` (ligne `SC_RUN_TEST` en plus de
  l'`#include`, nécessaire pour exécuter la suite).
- `tests/CMakeLists.txt` non modifié (hors périmètre) : la cible console `SpectroCraftTests` (non
  construite par le CI, `--target SpectroCraft`) devrait lister `src/AI/Sha256.cpp` hors du bloc IA et
  `src/AI/ModelCatalog.cpp` dans le bloc IA si on la fait revivre.
- Sous Windows, ctest et `--run-unit-tests` ne posent toujours pas `SC_MODELS_DIR` (tests/CMakeLists.txt,
  Main.cpp hors périmètre) : l'override est honoré mais personne ne s'en sert encore côté Windows.
- `verified` dans la liste = « SHA-256 vérifié dans ce processus » (pas de hachage pendant `list`) ;
  `false` ne veut pas dire corrompu : appeler `ai.models.verify`.

## Fusion : ce que le fil principal doit faire

Fichier proposé : `src/control/commands/Ai.cpp`, drapeaux `AiBuildOnly`. Tout le travail est dans le
catalogue ; chaque handler fait 5-15 lignes.

- `ai.models.list` (`Query`) → `Outcome::success (ModelCatalog::instance().listJson())`.
- `ai.models.install {modelId}` (`LongRunning`) :
  1. `auto plan = cat.installPlan (id); if (!plan.ok) return Outcome::failure (plan.code, plan.message, plan.details);`
  2. `jobs.start ({ "ai.models.install", false /*non mutant*/, … }, [id] (JobControl& jc) {
       auto o = ModelCatalog::instance().install (id, [&] (float f, const std::string& s)
                    { jc.progress (f, s); return !jc.cancelled(); });
       return o.ok ? Outcome::success (o.result) : Outcome::failure (o.code, o.message, o.details); });`
  3. réponse immédiate `{"job":…, "sizeBytes": plan.result.sizeBytes}`.
- `ai.models.verify {modelId}` : même patron avec `cat.verify`.
- `ai.models.remove {modelId, confirm}` (`Destructive`) : sans `confirm` →
  `confirm_required` avec `cat.removalPreview(id).result.message` ; refuser `busy` si un job IA
  (noiseSuppress/transcribe) utilise ce modèle (le catalogue ne connaît que ses propres opérations) ;
  puis `cat.remove(id)` (rapide pour OpenVINO/Whisper ; en job si on veut être prudent sur les gros GGUF).
- `ai.devices {init}` (`Query`) : `init:false` → `devicesJson (app.aiDevicePref, false)` direct ;
  `init:true` → job (charge OpenVINO), résultat `devicesJson (app.aiDevicePref, true)`.
- `ai.setDevice {engine, device, persist}` : `openvino` → `resolveAiDevice ("openvino", device)` dans un
  job si le runtime n'est pas prêt (ou `availableDevices(…, false)` si `ready()`), erreur →
  `Outcome::failure (r.code, r.message, r.errorJson()["details"])` ; succès → `app.aiDevicePref = r.requested`
  (`"auto"` ou nom), `persist` → `saveSettings`, résultat `r.toJson()` + `persisted`.
  `audiocpp` → mémoriser le backend à part (pas de réglage existant).
- **Actions.cpp** (lot 0, `f1cfa62`) : l'étape `noiseSuppress` utilise encore l'ancien
  `sc::resolveAiDevice` (repli CPU silencieux). Pour une commande API, appeler
  `sc::ai::resolveAiDevice ("openvino", pref)` et échouer sur `!r.ok` ; garder le repli pour le
  rejeu d'actions enregistrées si on veut la compatibilité.
- `--test-ns-failure-alert`, dialogues : `buildNsFailureDetails` inchangé (même déclaration).
- Conflits attendus au merge : `tests/UnitTests.cpp` (lignes voisines de `regression/ai.inc`),
  `src/CMakeLists.txt` (liste des sources) — triviaux.

## Dettes / suite

- Extraire toutes les DLL des zips CUDA (StemSeparation/SuperResolution/`installBinaryFromZip`), puis
  retirer `catalogBlocker`.
- Le verrou « busy » ne couvre pas les téléchargements lancés par les dialogues (premier usage NS /
  Whisper, setup Stem/SR) ; sous Windows deux téléchargements simultanés du même fichier partagent
  le même `.part` (préexistant).
- Migration audio.cpp (API C, backends cpu/vulkan/cuda/metal) : ajouter un `engine` et des entrées ;
  `resolveAiDevice` accepte déjà les backends comme données.
