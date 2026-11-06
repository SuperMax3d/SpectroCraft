# Lot 4 — mesures chiffrées exportables

Statut : **fait**. Le module de calcul `sc::measures` (extrait de `FFTAnalysisView`, égalité bit à bit
avec le panneau, nouvelles mesures levels/peaks/harmonics) a été livré par l'agent parallèle
`ai-control-measures` (voir `lot-4-core.md`) ; ce fichier couvre la fusion et les commandes.

## Fusion

`ai-control-measures` fusionnée à `8d4ace9` (conflit trivial `tests/UnitTests.cpp`, deux côtés gardés).

## Commandes (`src/control/commands/Measure.cpp`)

| Commande | Fonction | Notes |
|---|---|---|
| `measure.levels {start, end, channels, clipThreshold}` | `measures::levels` | RMS/crête/instant de crête/facteur de crête/DC/min/max/écrêtés par canal, `unit:"dBFS"`, `calibration{unit, fullScale[], levelOffsetDb[], calibrated[]}`, et si calibré `rmsAbsDb/peakAbsDb/absUnit/rmsPhysical/peakPhysical` |
| `measure.spectrum {…, fftSize, window, overlap, values, peaks{…}}` | `measures::spectrum` + `peaks` | défaut `fftSize` = celui du panneau (`settings.fftLocalN`) pour retrouver les chiffres de l'écran ; refus au-delà de 1 M valeurs inline |
| `measure.thirdOctave {…, mode, order, fftSize, window}` | `measures::thirdOctave` | défaut = choix du panneau : `reference` (dB-SPL) pour un fichier en Pascal, `iec` sinon ; bandes vides `-999` ; ordre ∉ {1,2,3,4,5,10,20,30,40} → `invalid_params` |
| `measure.harmonics {…, channel, fundamentalHz | anchorHz+n | auto, maxK, search, toleranceHz}` | `measures::harmonics` | THD si la fondamentale est valide ; pas de fondamentale → `failed` avec indice |
| `measure.curve {kind: rms|peak|ampVsTime|freqVsTime|ampVsFreq, …}` | `levelCurve` / `timeCurve` / `spectrumCurve` | `track`, `smooth`, `useSpectralSelection` (copie de la sélection spectrale) |
| `measure.probe {t, f, channel}` | `SpectralEngine::probeMagnitude` | thread de messages (cache de tuiles partagé avec les vues), réglages du spectrogramme |
| `measure.spectrogramData {…, fftSize, hop, fMax, format}` | `SpectralEngine::computeTile` sur une **instance locale** (aucun cache partagé : sûr sur un worker) | inline ≤ 1 M valeurs, sinon fichier `f32` (lisible par `numpy.fromfile(p,'<f4').reshape(frames,bins)`) ou `csv` dans `<temp>/SpectroCraft/control-out/` ; refus au-delà de 50 M valeurs |
| `export.spectrogram {path, start, end, width, height, overlays, confirm}` | `renderToPng` (instance locale) | job ; `confirm` si la cible existe ; **overlays → `needs_ui`** (voir écarts) |
| `markers.exportCsv {path, confirm}` / `markers.importCsv {path}` | `MarkerManager::saveCsv/loadCsv` | périmètre de chemins |

Exécution : petite région → inline sur le thread de messages ; grosse (> 20 M échantillons-canaux pour
`levels`, 5 M pour les analyses FFT), mode référence de 1/3 d'octave, spectrogramme, export → **job
exclusif** (`mutating:true`) : pendant qu'il lit le document, ni édition ni fermeture ne passent (garde
busy du registre) ; et un job de mesure refuse de démarrer à côté d'un job mutant (les commandes
`measure.*` sont `Query`, donc la garde générale ne les arrête pas). Le worker ne reçoit qu'une
référence à l'`AudioEngine` (lecture `readSamples`, thread-safe) et une **copie** de la calibration.

## Tests (`tests/regression/measure_commands.inc`, « regression: measure_commands »)

Sinus pleine échelle sur un bin exact (~1 kHz), fichier calibré 1 Pa :
`measure.levels` crête 0 dBFS, RMS −3,01 dBFS, **90,97 dB-SPL** ; `measure.spectrum` **identique à
`sc::measures::spectrum` (le code du panneau)** au 1/1000 dB près (arrondi JSON), bin du ton −3,01 dB,
pic le plus fort au ton ; `measure.thirdOctave` mode référence en job, unité dB-SPL, **bande 1 kHz
≈ 91 dB-SPL** (même référence que les tests dB-SPL du 2026-11-03), ordre invalide refusé ;
harmoniques ; courbes rms (−3,01 dBFS par fenêtre) et ampVsFreq ; probe ; spectrogramme inline et
fichier f32 de taille frames×bins×4 ; export PNG, `confirm_required`, `needs_ui` pour les overlays ;
aller-retour CSV des marqueurs ; job de mesure refusé pendant un job mutant.

Résultats (macOS arm64, Debug + IA) : « measure,control » 1018/1018 ; suite complète **5998/5998**.

## Écarts

- `export.spectrogram` avec overlays : la commande répond `needs_ui` **même dans l'app ouverte**.
  `MainWindow::preparePngExport` (`SpectralView::PngExportJob`) n'est pas encore exposé par `UiBridge` ;
  l'export sans overlays (rendu headless identique à `--test-spectral`) est disponible partout.
  À faire au lot 6 avec la migration de `FileExportSpectral`.
- Pas d'écriture `.npy` (optionnelle au plan) : `f32` + forme suffisent.
- `measure.probe` ne donne pas `frameSec` (la fonction de `SpectralEngine` ne l'expose pas).

## Consignes

- Les valeurs « de l'écran » supposent les mêmes réglages que le panneau (taille FFT, fenêtre) : les
  défauts des commandes lisent les réglages courants de l'app.
