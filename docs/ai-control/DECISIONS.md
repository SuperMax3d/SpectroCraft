# Phase 10 — exécution du plan de l'audit « contrôle par IA »

Démarré le 2026-11-05. Branche `ai-control` du fork `nicolasmarcotteTaintin/SpectroCraft`, créée depuis le `main` de
SuperMax3d (`8c527e3`). Poussée sur le fork seulement ; aucune PR vers SuperMax3d sans accord.

- Rapport de référence : `docs/ai-control/RAPPORT.md` (§5 architecture, §6 protocole,
  §7 MCP, §8 lots).
- Build : macOS local (`SpectroCraft-build/macos`, hors dépôt) ; agent cloud : Linux (voir CI `linux-ai`).
- Une note de passation par lot : `docs/ai-control/progress/lot-N.md` (fait, écarts, dettes, consignes pour la suite).

## Décisions retenues (recommandations du rapport, §9)

| Question | Choix |
|---|---|
| Q1 ordre lots 3/4 | ordre du rapport (IA puis mesures) |
| Q2 opt-in | désactivé par défaut ; pas de toast |
| Q3 fichier de découverte | `<userApplicationDataDirectory>/SpectroCraft/control/<pid>.json` |
| Q4 adaptateur | Python 3.11+, `uv` ou `pip install -e` |
| Q5 outils MCP | outils typés + outils dynamiques |
| Q6 journal persistant | oui (lot 5) |
| Q7 édition après undo | acceptée, signalée par `redoDiscarded` |
| Q8 fichier streamé | matérialisé dans un job ; anciens flags conservés |
| Q9 migration des menus | faite (lot 6) |
| Q11 protocole | `protocol: 1`, expérimental |
| Q12 correctifs | commit séparé en tête du lot 0 |
| Q14 option CMake | `SC_ENABLE_CONTROL` ON par défaut |
| Q15 `--control` × `multi_instance=0` | seconde instance autorisée |
| Q16 langue | identifiants/messages en anglais, docs en français |
| Q18 périmètre de chemins | dossier personnel + `scriptFolder()` + temporaire |
| Q10, Q13, Q17 | laissées à Nicolas/Max (non bloquantes pour le code) |
