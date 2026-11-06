# Lot 4 (cœur) — module de mesures headless `sc::measures`

Branche `ai-control-measures` (fork `nicolasmarcotteTaintin/SpectroCraft`), créée depuis `ai-control`
(`76de144`, voir « Écarts »). Travail d'un agent parallèle : il ne contient **aucune** commande
`measure.*` ; il fournit le calcul, partagé avec le panneau FFT, sur lequel l'agent principal
n'aura qu'à écrire des enveloppes minces quand il arrivera au lot 4.

## Fait

- **Module `sc::measures`** (`include/Measures.h`, `src/Measures.cpp`) : fonctions libres, sans
  `Component`, sans géométrie, sans `app.selection`. Entrées : `const AudioEngine&` (lecture par
  `readSamples`, thread-safe), canaux, région en secondes, taille FFT, type de fenêtre, recouvrement,
  calibration (`const ScaleCalibration*`, copie par valeur côté appelant). Sorties : structures simples
  (fréquences, niveaux, chaînes d'unités). Les calculs longs prennent un `ProgressFn`
  (`bool(double fraction)`, `false` = annuler) et signalent `cancelled`.
- **Extraction depuis `FFTAnalysisView`, arithmétique inchangée** (code déplacé tel quel) :
  - `computeRegionFrom` → `regionSpectrumNorm(eng, chans, t0, t1, n, windowType, …)` : le type de
    fenêtre est un argument au lieu d'être lu dans `windowCombo` ; le placement des fenêtres est
    isolé dans `spectrumWindowStarts` ;
  - corps de `thirdOctBandsRef` → `thirdOctaveRefBands` (même signature + progression ;
    `FFTAnalysisView::thirdOctBandsRef` reste, signature et résultats identiques, et délègue) ;
  - helpers `static` de fichier sortis de `Panels.cpp` : `makeAnalysisWindow`, `windowEnbwBins`,
    `toThirdOctBands`, `bandsNormalizedToDb`, `curveFrameStarts`, tables de centres IEC/référence,
    constantes `kMaxAvgWindows`, `kMaxCurveFrames`, `kTimeModeFFT`, `kNormFloorDb` ;
  - `computeTimeCurveFrom` / `computeSpectrumCurveFrom` → fonctions libres `timeCurve` /
    `spectrumCurve` (supprimées de la classe) ; `smoothSeries` partagé ;
  - `FFTAnalysisView::CurveData` est désormais un alias de `sc::measures::CurveData`.
- **Le panneau utilise le module** : spectre live et snapshots, bandes IEC, pipeline dB-SPL, courbes
  live et slots de courbes, et la table « Copy Harmonics » (`buildHarmonicsTable` passe par
  `harmonics(..., Search::NearestBin)`, même bin, même valeur). Aucun changement de comportement :
  cache, throttle, clés de cache, dessin intacts.
- **Nouvelles mesures** :
  - `levels()` : RMS, crête (+ instant), facteur de crête, offset DC, min/max, nombre d'échantillons
    écrêtés, par canal, en dBFS et, si calibré, en absolu (`dB-SPL`/`dBV`/`dB/g`) et en valeur
    physique (Pa/V/g) ; pleine échelle **par canal** (`fullScaleFor`/`levelOffsetDb`) ;
  - `levelCurve()` : RMS ou crête en fonction du temps (fenêtre/pas en secondes), pour
    `measure.curve` `kind:"rms"|"peak"` ;
  - `findPeaks()` / `peaks()` : pics spectraux (maxima locaux, plateaux = premier bin), proéminence
    topographique (définition `scipy.signal.peak_prominences`, O(n log n)), tri par proéminence,
    top-N, fenêtre de fréquences, fréquence et niveau affinés par parabole (domaine dB) ;
  - `harmonics()` : fondamentale donnée, ou ancre/N (sémantique du panneau), ou auto-détectée à
    partir des pics (candidats à moins de 40 dB du plus fort, score = puissance cumulée des pics
    tombant sur ses multiples) ; harmoniques 1..N, recherche du max dans ±tolérance (défaut
    max(2 bins, 3 % de f0), plafonnée à 0,45·f0) ou bin le plus proche ; niveau relatif à la
    fondamentale, THD (%), harmoniques au-delà de Nyquist marquées `valid=false`.
  - `spectrum()` / `thirdOctave()` : enveloppes « résultat complet » (métadonnées, calibration par
    canal, unités) autour des fonctions bas niveau ; `spectrum()` accepte un `overlap` (≥ 0 : pas
    fixe n·(1−overlap) ; < 0 : placement du panneau, défaut).

## Fichiers

| Fichier | Changement |
|---|---|
| `include/Measures.h`, `src/Measures.cpp` | nouveaux (module) |
| `src/Panels.cpp`, `include/Panels.h` | le calcul est remplacé par des appels au module (−528 lignes nettes) |
| `src/CMakeLists.txt` | + `Measures.cpp` (cible app) |
| `tests/CMakeLists.txt` | + `Measures.cpp` (cible console `SpectroCraftTests`, sinon elle ne lie plus : `Panels.cpp` en dépend) |
| `tests/regression/measures.inc` | nouveaux tests |
| `tests/UnitTests.cpp` | 2 lignes : `#include "regression/measures.inc"` + `SC_RUN_TEST ("regression: measures", …)` |

Rien d'autre (ni `Application.cpp`, `Actions.cpp`, `Main.cpp`, `MainWindow.cpp`, `src/control/**`,
`tools/**`, CI).

## Tests et résultats

`tests/regression/measures.inc` (« regression: measures », 9 tests, 145 vérifications) :

- **Égalité exacte (bit à bit, `==` sur les `std::vector<float>`) panneau ↔ module** pour la même
  région et les mêmes réglages : spectre (3 fenêtres, N = 1024…8192, région multi-fenêtres,
  point, fichier entier, région courte en fin de fichier, 2 canaux) ; bandes IEC (Hann/Hamming,
  N = 1024/4096) ; bandes dB-SPL de référence (ordres 40 et 10, fichier Pascal) ; courbes des modes
  3/4/5 × suivi Max/Total/Weighted ; table « Copy Harmonics » (format et valeurs).
- **Signaux connus** : sinus pleine échelle → crête 0 dBFS, RMS −3,01 dBFS (RMS = crête − 3,01 dB),
  90,97 dB-SPL à 1 Pa ; bin du spectre à −3,01 dB / 90,97 dB-SPL ; bande 1 kHz ≈ 91 dB-SPL (même
  référence que le test dB-SPL existant) ; DC inclus dans le RMS et rapporté à part ; pleines échelles
  par canal ; silence → plancher −300 ; courbe RMS d'un échelon (−9,03 puis −23,01 dBFS) ; deux tons
  hors bin trouvés à moins d'un demi-bin, niveaux à ±0,5 dB ; proéminences et plateau sur un spectre
  synthétique ; série harmonique 441,7 Hz × 6 : fondamentale auto-détectée et harmoniques à moins
  d'un demi-bin, niveaux à ±0,5 dB, offset dBV, THD ≈ 70,1 % ; ancre/N et Nyquist ; NearestBin.
- **Annulation / progression / threads** : progression monotone jusqu'à 1 ; annulation de
  `spectrum`, `levels`, `thirdOctaveRefBands`, `thirdOctave`, `timeCurve` ; deux `spectrum()`
  concurrents sur des `std::thread` donnent exactement le résultat du thread principal ; plafond de
  2048 fenêtres signalé ; recouvrement 50 % → 92 fenêtres.
- **Non-régression du panneau vérifiée hors dépôt** : comme le panneau appelle maintenant le module,
  l'égalité ci-dessus est structurelle. J'ai donc aussi comparé, une fois, des **copies verbatim des
  fonctions d'avant refactorisation** (extraites de `ai-control`) au module, sur ~1 300 cas aléatoires
  (3 fréquences d'échantillonnage, N = 128…65536, 3 fenêtres, régions aléatoires dont plafonnées,
  point, fin de fichier, 3 suivis, bandes IEC, bandes de référence ordres 40/3, table d'harmoniques) :
  **0 écart** (égalité bit à bit). Ce test temporaire n'est pas versionné (il dupliquait 400 lignes
  de l'ancien code, et des valeurs « dorées » ne seraient pas portables : le FFT AVX2 et le repli JUCE
  ne donnent pas les mêmes bits). La preuve de non-régression du panneau repose donc sur cette
  comparaison ponctuelle **et** sur les tests existants du panneau (dB-SPL, bandes, normalisation),
  inchangés et verts ; les tests d'égalité versionnés, eux, garantissent seulement que panneau et
  module restent branchés sur le même code.
- **Suite complète** (macOS arm64, Debug, AI OFF, FFmpeg OFF, `ctest -j 8`) : voir la section
  « Résultats de la suite » en fin de fichier.
- Les tests dB-SPL / 1/3 d'octave existants (`tests/UnitTests.cpp` « 1/3 octave dB-SPL »,
  `regression/spectral_dsp.inc` W4, `regression/panels.inc` band sum) passent **sans modification**.

## Écarts

- **Base** : la consigne disait `ai-control` à `5a80afd` ; ce commit n'est sur aucune branche du fork,
  `fork/ai-control` pointe sur `76de144` (même message, seule différence : deux lignes de
  `DECISIONS.md`). Branché sur `76de144` pour que la fusion soit une avance rapide côté historique.
- `tests/CMakeLists.txt` modifié (une ligne) : obligatoire pour que la cible console
  `SpectroCraftTests` continue de lier (le CI ne la construit pas, mais elle existe).
- `tests/UnitTests.cpp` : deux lignes au lieu d'une (l'`#include` **et** le `SC_RUN_TEST`, sans quoi
  le fragment n'est jamais exécuté).
- Build et tests faits sur **macOS** (machine de Nicolas), pas sur Linux : l'agent tournait sur le Mac.
  Le code n'utilise que du C++20 standard + JUCE (pas de construction spécifique à une plateforme ;
  `int64_t` partout, comme le code d'origine).
- `measure.probe` et `measure.spectrogramData` ne sont **pas** dans le module : ils correspondent
  déjà à des fonctions headless de `SpectralEngine` (voir la fiche).

## Revue indépendante (sous-agent) et correctifs

Un relecteur indépendant a confirmé le déplacement verbatim (diff des corps de fonctions) et relevé
des défauts dans les **nouvelles** fonctions, corrigés dans le 2e commit, chacun avec un test :

- `levels` / `levelCurve` : `t1 = durée` tronquait parfois au dernier échantillon près
  (`(int64_t)(durée·fs) = N−1` pour ~7 % des longueurs) → `sampleAt()` renvoie `N` en fin de fichier ;
- `harmonics` : THD de 300 % sur du silence → pas de THD (`< 0`) quand la fondamentale est au plancher ;
- `levelCurve` : région vide → aucune valeur (au lieu d'un −300) ; fenêtre plus longue que la région →
  lecture par blocs (plus d'allocation de la taille de la région) ;
- `detectFundamental` : un pic plus faible à f0/2 l'emportait (erreur d'octave) → score pondéré par la
  proportion d'emplacements harmoniques remplis (test : échoue sans le correctif, passe avec) ;
- validation : type de fenêtre hors 0..2 et ordre hors {1,…,40} rejetés (résultat sans canal) au lieu
  d'être corrigés en silence ; `overlap` NaN = placement du panneau ; bornes de fréquence énormes de
  `findPeaks` bornées avant conversion en `int` ;
- tests : délai de `scRecomputeUntil` porté à 10 s (machines Debug/CI lentes).

Non corrigé (documenté) : les accesseurs `numSamples()/duration()/sampleRate()` d'`AudioEngine` ne
sont pas synchronisés ; un job de mesure pendant un enregistrement ou un rechargement est une course
formelle → c'est à la garde busy du registre de l'empêcher (voir « Durée de vie » ci-dessous).

## Fiche API pour les enveloppes `measure.*` (§6.6)

Règles communes :

- **Thread** : sur le thread de messages, résoudre le document puis **copier** ce dont le calcul a
  besoin : `const AudioEngine& eng = doc->engine`, `ScaleCalibration cal = doc->scale` (copie par
  valeur), liste de canaux, et pour les courbes une **copie** de `doc->spectralSel` si non vide. Les
  mesures courtes (une fenêtre, `levels` sur quelques secondes) peuvent être exécutées inline ; les
  autres sont des **jobs** (drapeau `LongRunning`), le `ProgressFn` relaie la progression et
  l'annulation du job.
- **Durée de vie** : le module ne possède rien. Pendant un job, le document ne doit être ni fermé
  (`closeDoc` fait `docs.erase`) ni modifié (édition, resample : `readSamples` est sûr mais
  `sampleRate()/duration()/numSamples()` ne sont pas synchronisés) → la garde busy de `invoke()` doit
  refuser `doc.close` et les commandes `Mutates` sur ce document tant qu'un job de mesure tourne (ou
  annuler et attendre le job avant).
- **Calibration** (à recopier dans chaque résultat absolu) : `ChannelCalibration { calibrated,
  fullScale, levelOffsetDb }` par canal ; `absUnitFor(&cal)` → `"dB-SPL"|"dBV"|"dB/g"|""` ;
  `physicalUnitFor(&cal)` → `"Pa"|"V"|"g"|""` ; `ScaleCalibration::unitName(cal.unit)` →
  `"Pascal"|"Volts"|"g"|"None"`. N'émettre les champs `abs*` que si `calibrated`.
- **Conventions** : `unit:"dBFS"` = 0 dB pour une **crête** |1,0| ; spectres en RMS mono-latéral
  corrigé du gain cohérent (sinus pleine échelle = −3,01 dB) ; absolu = relatif + `levelOffsetDb(ch)`.
  Plancher −300 = silence numérique (`levels`, `levelCurve`, harmoniques invalides) ; plancher −160
  pour les spectres/courbes (`kNormFloorDb`). Fenêtres JSON : `windowTypeFromName("hann"|"hanning"|
  "hamming"|"rect"|"rectangular")` (−1 → `invalid_params`), `windowName(type)` en sortie.
- **Validation** à faire dans l'enveloppe : `fftSize` puissance de 2 dans [2, 65536] (le module rend
  un résultat vide sinon), `order` ∈ {1,2,3,4,5,10,20,30,40}, `start ≤ end`, canaux existants
  (le module ignore silencieusement les canaux invalides ; mieux vaut `invalid_params`).
- **Gros volumes** : au-delà de 1 M valeurs (spectre 65536 × canaux, courbes, spectrogramme), écrire
  dans un fichier (`inline:false`, `path`, format `f32`/`csv`) comme prévu en §6.6.

| Commande | Fonction(s) | Paramètres → arguments | Champs JSON à renvoyer |
|---|---|---|---|
| `measure.levels` | `levels(eng, LevelsParams{t0,t1,channels,clipThreshold}, &cal, progress)` | `start`/`end` (absents = fichier entier : `t1 ≤ t0` ⇒ jusqu'à la fin), `channels` (absent = tous) | `region{start:r.t0,end:r.t1,samples:r.s1-r.s0}`, `unit:"dBFS"`, `calibration{unit:physicalUnit, fullScale:[…], levelOffsetDb:[…], calibrated:[…]}`, `channels[{ch, rmsDb, peakDb, peakAtSec, crestDb, dcOffset, minSample, maxSample, clipped, samples` + si calibré `rmsAbsDb, peakAbsDb, absUnit, rmsPhysical, peakPhysical}]` |
| `measure.spectrum` | `spectrum(eng, SpectrumParams{t0,t1,channels,fftSize,windowType,overlap}, &cal, progress)` ; si `peaks` demandé : `peaks(spec, PeakParams{maxCount,minProminenceDb,minLevelDb,minFreqHz,maxFreqHz})` | défauts recommandés = réglages du panneau du document (`settings.fftLocalN`, fenêtre Hann) pour obtenir « les chiffres de l'écran » ; `overlap` absent = placement du panneau | `analysis{fftSize, window:windowName, enbwBins, framesAveraged, framesCapped, binHz, sampleRate, levelConvention, unit:"dBFS"}`, `channels[{ch, relDb[], absDb[]?, absUnit?, calibrated, fullScale, levelOffsetDb}]` (`relDb` = n/2+1 valeurs, bin k ↔ k·binHz), `peaks[{ch, freqHz, relDb, absDb?, prominenceDb, bin, binRelDb}]` |
| `measure.thirdOctave` | `thirdOctave(eng, ThirdOctaveParams{t0,t1,channels,mode,order,fftSize,windowType}, &cal, progress)` | `mode:"reference"` (pipeline dB-SPL : 65536 Hanning 50 %, Butterworth ordre 2·`order`) ou `"iec"` (26 bandes sommées depuis le spectre, ENBW) ; défaut recommandé = celui du panneau : `reference` si le document est en Pascal, sinon `iec` | `mode`, `window` (`"hanning-65536"` en référence, sinon `windowName-fftSize`), `order`, `unit` (= `absUnit` si calibré, sinon `"dB"`), `levelOffsetDb` par canal, `framesUsed`, `framesCap`, `bands[{fcHz:centresHz[b], relDb, absDb?}]` par canal, `emptyBandValue:-999`. Si `noData[b]` : écrire `-999` pour `relDb` **et** `absDb` (la valeur brute stockée est −999 + 20·log10(2e-5)). En mode référence, absolu = `relDb + levelOffsetDb` (= niveau de l'analyseur de référence) |
| `measure.curve` | `kind:"freqVsTime"\|"ampVsTime"` → `timeCurve(eng, chans, t0, t1, track, mask, out, progress)` ; `kind:"ampVsFreq"` → `spectrumCurve(eng, chans, t0, t1, kTimeModeFFT, Hann, track, mask, out, progress)` ; lissage → `smoothSeries(v, window)` (amplitude ; fréquence aussi hors ampVsFreq, comme le panneau) ; `kind:"rms"\|"peak"` → `levelCurve(eng, ch, t0, t1, windowMs/1000, hopMs/1000, LevelKind, progress)` | `track` : `"max"\|"total"\|"weighted"` → 0/1/2 ; `mask` = copie de la sélection spectrale (ou `nullptr`, option `useSpectralSelection`) | courbes : `{kind, track, unit:"dBFS", channels[{ch, x[], ampDb[], freqHz[]}]}` (`x` en s ou Hz) ; rms/peak : `{kind, unit:"dBFS", t0, hopSec, windowSec, count, values[]}` |
| `measure.probe` | hors module : `app.spectralEngine().probeMagnitude(eng, ch, t, f, settings, 0)` (non const, cache partagé de tuiles → **thread de messages**) | `t`, `f`, `channel` ; FFT/hop/fenêtre = réglages du spectrogramme (`app.settings`) | `{t, f, binHz: rate/fftSize, fftSize, magDb, unit:"dBFS", absDb?: magDb+levelOffsetDb(ch), absUnit?}` (`frameSec` du §6.6 : non fourni par `probeMagnitude`, l'omettre ou le recalculer) |
| `measure.spectrogramData` | hors module : `SpectralEngine::computeTile(eng, ch, startSample, numSamples, fftSize, hop, windowType)` (const, sans cache, utilisable depuis un worker) tuile par tuile dans un job | `fMax` → tronquer à `ceil(fMax/binHz)` bins | `{frames, bins, binHz, hopSec, unit:"dBFS", inline, format:"f32"\|"csv", path, layout:"frames x bins, float32 little-endian, C order"}` |
| `measure.harmonics` | `spectrum(...)` puis `harmonics(spec, 0, HarmonicsParams{fundamentalHz \| anchorHz+anchorN, maxK, search, toleranceHz, detect})` | `anchorHz`+`n` = sémantique du panneau (l'ancre est l'harmonique n°n) ; `fundamentalHz` explicite ; rien = auto-détection ; `search:"peak"\|"nearestBin"` (le panneau utilise `nearestBin`) | `{fundamentalHz, autoDetected, toleranceHz, unit, fftSize, window, harmonics[{k, expectedHz, freqHz, relDb, absDb?, relToFundamentalDb, valid, isLocalMax}], thdPercent}` (`fundamentalHz:0` → erreur `not_found` conseillée) |

## Consignes pour l'agent principal (fusion)

1. Fusionner `ai-control-measures` dans `ai-control` (conflits attendus seulement sur
   `src/CMakeLists.txt` — une ligne ajoutée après `SpectralEngine.cpp` — et sur les deux lignes de
   `tests/UnitTests.cpp` si le lot 0/1 ajoute aussi des fragments : garder les deux).
2. Écrire `src/control/commands/Measure.cpp` avec les enveloppes de la fiche ; aucun calcul à ajouter
   hors `probe`/`spectrogramData`. Marquer `thirdOctave` (référence), `spectrum` multi-fenêtres,
   `curve`, `spectrogramData` comme `LongRunning` ; `levels` aussi au-delà de quelques secondes.
3. Ajouter à la garde busy le refus de `doc.close` / commandes mutantes sur un document qu'un job
   de mesure lit (voir « Durée de vie »).
4. Tests d'enveloppe : réutiliser les signaux de `measures.inc` (sinus pleine échelle à 1 Pa →
   `rmsAbsDb` 90,97 ; bande 1 kHz ≈ 91 dB-SPL) et vérifier les champs JSON/unités.
5. Le §2.2 du rapport (« RMS local `static` à `MainWindow.cpp:4358` », peak inline dans
   `normalize`) n'a pas été touché : ces fichiers appartiennent à d'autres agents. On pourra les faire
   passer par `measures::levels` plus tard si on veut une seule définition.

## Résultats de la suite

macOS 26 arm64, AppleClang, Debug, `-DSPECTRO_ENABLE_AI=OFF -DSPECTRO_ENABLE_FFMPEG=OFF`,
`ctest --test-dir build -j 8` (8 shards) :

| Exécution | Résultat |
|---|---|
| Base (`ai-control` intact) | 1 échec : `views#W4: a 5 px repaint draws the same selection pixels as a full paint` (shard 1) ; le test « regression: views » seul repasse (33/33) → **instable sous charge, préexistant** |
| Branche, commit 1, 1re exécution | 10/10 shards verts |
| Branche, commit 1, 2e exécution (`-V`) | 4747/4748 vérifications ; seul échec : le même `views#W4` instable |
| Branche, commit 2 (correctifs de revue, `-V`) | **4760/4760**, 10/10 shards verts |
| `SC_TEST_FILTER="regression: measures"` | 145/145 |
| CI GitHub Actions `build.yml` sur `f0fe363` (run 42154351288, AI ON) | **windows-ai ✓, macos-ai ✓, linux-ai ✓** (MSVC, AppleClang, GCC) |

Aucune régression par rapport à la base. Le seul échec observé est l'instabilité préexistante de
`views#W4` (rendu de sélection, sans lien avec les mesures).
