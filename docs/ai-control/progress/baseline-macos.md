# Référence de tests avant tout changement (macOS)

Date : 2026-11-05. Branche `ai-control`, tip `76de144` (= `main` de SuperMax3d `8c527e3` + commit de docs).

## Écart par rapport à la consigne

La consigne prévoyait un agent cloud Linux (`baseline-linux.md`, build sous `xvfb-run`). L'exécution a en
fait tourné sur le Mac de Nicolas (MacBook Air M-series, macOS 26.6, Xcode 26.6, AppleClang 21, arm64),
dans un worktree isolé. Pas de Docker ni de VM Linux disponibles : **aucun build Linux local**. Linux et
Windows sont couverts par le CI (`gh workflow run build.yml`, voir les notes de chaque lot).

## Configuration

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSPECTRO_ENABLE_AI=ON -DSPECTRO_ENABLE_FFMPEG=OFF \
  -DSC_AI_DOWNLOAD_DIR=<SpectroCraft-build>/ai-downloads -DSC_TEST_MODELS_DIR=<SpectroCraft-build>/test-models
cmake --build build --target SpectroCraft --parallel 8
ctest --test-dir build -j 8 -V
```

Archives OpenVINO 2024.6 (arm64) et whisper.cpp b5454 déjà téléchargées (cache local), modèles de test
`denseunet`, `deepfilternet2`, `whisper-base` déjà présents : les tests IA ont tourné réellement.

## Résultat

- Build : OK, sans erreur.
- ctest : **10/10** (2 fixtures + 8 shards), 67 s.
- Vérifications : **4859/4859 checks passed** (somme des 8 shards).
- 9 tests ou parties sautés, tous attendus sur macOS :
  - change gain dialog / drawing key shortcuts / scale dialog focus : focus clavier exigeant le premier plan OS ;
  - playback chain, rendering backend : Windows seulement ;
  - scfft AVX2 (×2) : pas d'AVX2 sur arm64 ;
  - super-res cli args : CLI audio.cpp Windows seulement ;
  - video audio extraction : build sans FFmpeg.

Aucun échec préexistant.
