# ESP32-audioI2S (v3.4.5d_SLD)

# Changes applied to ESP32-audioI2S (v3.4.5d)

This document provides a detailed breakdown of the modifications, improvements, and bug fixes applied to the library. Each change aims to optimize resource usage (Flash, RAM, CPU) and improve system stability.

### 1. Modular Configuration

1) Codec Selection
**Context:** By default, the library compiles all decoders and features, consuming significant Flash memory even if only one format is used.
**Improvement:**
- **Codec Selection:** Selective enable/disable via `#define` macros in `Audio.h`.
- **OGG Pack:** Enabling `AUDIO_CODEC_OGG` activates the OGG container and the **Opus** decoder (modern high-quality streams). **Vorbis** support can be added optionally.
- **M4A/AAC:** Enabling `AUDIO_CODEC_M4A` (container) automatically activates the **AAC** decoder, as M4A files almost exclusively contain AAC-encoded audio.

2) Granular Log Control
**Improvement:** Added `AUDIO_LOG_LEVEL` macro (0-4) to strip log strings from the binary at compile-time.
**Benefit:** Saves 10-15KB of Flash. Since logs are handled by the preprocessor, format strings are physically removed from the final binary if the level is disabled.

3) VU-Meter Management
**Improvement:** Added `AUDIO_ENABLE_VUMETER` macro to enable or disable real-time peak level and spectrum calculations.
**Benefit:** Saves ~3-5% CPU when disabled, freeing up resources for audio decoding.

4) Dynamic EQ Headroom
**Improvement:** Added `AUDIO_EQ_HEADROOM_MODE` macro to control volume compensation during EQ boosts (0: Full, 1: 50%, 2: Disabled/Max Power).
**Benefit:** Allows users to choose between full audio protection or maximum output loudness.

5) Adjustable Volume Ramp Speed
**Improvement:** Added `#define AUDIO_VOLUME_RAMP_STEPS` in `Audio.h`.
**Benefit:** Controls the "fade" speed during Mute or volume changes.
- **Value 50 (Default):** Smooth standard fade.
- **Value 25:** Doubles the response speed (more reactive).

6) Modular File System Support (FS)
**Improvement:** Added `AUDIO_ENABLE_FS` macro to conditionally include or exclude all local file processing code (SD, LittleFS).
**Benefit:** Removes unnecessary dependencies and significantly reduces the final binary footprint.

7) Modular Speech Features (TTS) & String Elimination
**Context:** TTS features heavily relied on the Arduino `String` class, causing heap fragmentation.
**Improvement:** Added `AUDIO_ENABLE_SPEECH` macro and completely rewrote `openai_speech()` using `const char*` and `ps_ptr` buffers.
**Benefit:** Eliminates heap fragmentation risks and saves Flash memory when the module is disabled.

**Global Benefit:** Drastic reduction in resource usage. Combined, these optimizations can save up to **~487KB of Flash memory**.


### 2. Robust I2S State Management
**Context:** `E (10013)` errors often appeared during rapid zapping or station changes because the library tried to disable an already stopped I2S channel.
**Improvement:**
- Secured `I2Sstop()` by synchronizing the software state (`m_f_i2s_channel_enabled`) with the hardware.
- The software flag only resets to `false` if the hardware confirms a successful stop (`ESP_OK`).
**Benefit:** Eliminates `E (10013)` errors and ensures reliable hardware/software synchronization.

### 3. Restored "v3.4.4" Volume Curves
**Context:** Standard volume curves can feel unnatural or too slow at low levels.
**Improvement:**
- Re-implemented **Square** (Quadratic) and **Logarithmic** (authentic v3.4.4) formulas.
- **Mode 0 (Square):** Punchy and very responsive.
- **Mode 1 (Log):** Natural feel, faithful to the v3.4.4 version.
- **Switch:** A `#define CurveOff345` macro is available in `Audio.h` to force the use of the original v3.4.5 polynomial curves if preferred.
**Benefit:** Offers an immediate power increase and a more natural auditory feeling.

### 4. Bypass DSP Dynamique
**Context:** The IIR filter chain (Equalizer) consumes CPU cycles for every sample, even when gains are neutral (0 dB).
**Improvement:** Automatic detection of the neutral EQ state to bypass the entire calculation routine.
**Benefit:** 10-15% CPU savings during flat playback.

### 5. Fréquences EQ Dynamiques
**Context:** EQ cutoff frequencies were previously hardcoded.
**Improvement:** Added `setToneFrequencies()` and `getToneFrequencies()` methods to adjust cutoff frequencies at runtime.
**Benefit:** Allows dynamic sound signature adjustments to match the specific audio hardware.

### 6. Gestion EQ Anti-Clic
**Context:** In the original library, every gain change triggered a brutal filter reset, causing audible "clicks".
**Improvement:** Completely redesigned `IIR_calculateCoefficients()` with a state-tracking system (`m_last_eq`) and removed the global filter reset.
**Benefit:** Perfectly smooth sound transitions during adjustments.

### 7. Stratégie Headroom EQ
**Context:** EQ boosts can saturate the signal. The original library reduced global volume to compensate for every boost.
**Improvement:** Added `AUDIO_EQ_HEADROOM_MODE` to choose the severity of this compensation (0: Full, 1: Hybrid, 2: Disabled).
**Benefit:** Total flexibility between audio protection and raw power.

### 8. Performance & Memory Optimizations
**Improvement:** Low-level optimizations to maximize ESP32 efficiency.
- **FPU Optimization:** Forced usage of the 32-bit hardware floating point unit.
- **Constexpr:** Internal lookup tables moved to Flash memory (PROGMEM).
- **Buffer Persistent:** Persistent `ps_ptr` buffer for 48kHz resampling.
- **STL Removal:** Replaced `std::string` with C-style strings.
**Benefit:** Overall performance gain, reduced heap stress, and control over the DSP sound signature.

> ℹ️ **For detailed technical implementation of these features, please refer to [technic.md](./technic.md).**

***

# Changements appliqués à ESP32-audioI2S (v3.4.5d)

Ce document détaille les modifications, améliorations et corrections apportées à la bibliothèque. Chaque changement vise à optimiser l'utilisation des ressources (Flash, RAM, CPU) et à améliorer la stabilité du système.

### 1. Configuration Modulaire

1) Sélection des Codecs
**Contexte :** Par défaut, la bibliothèque compile tous les décodeurs et fonctionnalités, consommant beaucoup de mémoire Flash même si un seul format est utilisé.
**Amélioration :**
- **Sélection des Codecs :** Activation sélective via des macros `#define` dans `Audio.h`.
- **Pack OGG :** L'activation de `AUDIO_CODEC_OGG` active le conteneur OGG et le décodeur **Opus** (haute qualité moderne). Le support **Vorbis** peut être ajouté en option.
- **M4A/AAC :** L'activation de `AUDIO_CODEC_M4A` (conteneur) active automatiquement le décodeur **AAC**, car les fichiers M4A contiennent presque exclusivement de l'audio encodé en AAC.

2) Contrôle Granulaire des Logs
**Amélioration :** Ajout de la macro `AUDIO_LOG_LEVEL` (0-4) pour retirer les messages de log du binaire à la compilation.
**Bénéfice :** Économie de 10 à 15 Ko de Flash. Les logs étant gérés par le préprocesseur, les chaînes de caractères (textes des messages) sont physiquement retirées du binaire final lors de la compilation si le niveau est désactivé. Si un message n'est pas compilé, il n'occupe aucun octet en mémoire Flash.

3) Gestion du VU-Mètre
**Amélioration :** Ajout de la macro `AUDIO_ENABLE_VUMETER` pour activer ou désactiver les calculs de spectre et de niveaux en temps réel.
**Bénéfice :** Économie de ~3-5% CPU si désactivé, libérant des ressources pour le décodage audio.

4) Headroom EQ Dynamique
**Amélioration :** Ajout de la macro `AUDIO_EQ_HEADROOM_MODE` pour contrôler la compensation de volume lors d'un boost EQ (0: Totale, 1: 50%, 2: Désactivée/Puissance Max).
**Bénéfice :** Permet de choisir entre une protection totale contre la saturation ou une puissance sonore maximale.

5) Vitesse de Rampe du Volume Ajustable
**Amélioration :** Ajout la macro `#define AUDIO_VOLUME_RAMP_STEPS` dans `Audio.h`.
**Bénéfice :** Permet de régler la vitesse du "fondu" (Fade in/out) lors du Mute ou des changements de volume.
- **Valeur 50 (Défaut) :** Fondu standard et fluide.
- **Valeur 25 :** Double la vitesse de réaction (plus réactif).

6) Modularité du Système de Fichiers (FS)
**Amélioration :** Ajout de la macro `AUDIO_ENABLE_FS` permet d'exclure physiquement tout le code lié aux fichiers locaux (SD, LittleFS).
**Bénéfice :** Supprime les dépendances inutiles et allège considérablement le binaire final.

7) Modularité TTS et Suppression de String
**Contexte :** Les fonctions de synthèse vocale utilisaient la classe Arduino `String`, provoquant une fragmentation de la mémoire (tas/heap) à cause des concaténations dynamiques.
**Amélioration :** Ajout de la macro `AUDIO_ENABLE_SPEECH` pour désactiver complètement le TTS.
- **Refactorisation :** Réécriture complète de `openai_speech()` en utilisant des `const char*` et `snprintf` avec des buffers `ps_ptr`.
**Bénéfice :** Élimination des risques de fragmentation mémoire et économie de Flash si le module est désactivé.

**Bénéfice Global :** Réduction drastique de l'utilisation des ressources. Cumulées, ces optimisations permettent d'économiser jusqu'à **~487 Ko de Flash**.


### 2. Gestion d'État I2S Robuste
**Contexte :** Des erreurs `E (10013)` apparaissaient fréquemment lors du zapping ou du changement de station car la bibliothèque tentait de désactiver un canal I2S déjà arrêté.
**Amélioration :**
- Sécurisation de `I2Sstop()` en synchronisant l'état logiciel (`m_f_i2s_channel_enabled`) avec le matériel.
- Le flag logiciel ne repasse à `false` que si le matériel confirme l'arrêt effectif (`ESP_OK`).
**Bénéfice :** Élimine les erreurs `E (10013)` lors des changements rapides et garantit une synchronisation fiable.


### 3. Restauration des Courbes de Volume "v3.4.4"
**Contexte :** Les courbes de volume standard peuvent paraître peu naturelles ou trop lentes à bas volume.
**Amélioration :**
- Ré-implémentation des courbes **Square** (Quadratique) et **Logarithmique** (authentique 3.4.4).
- **Mode 0 (Square) :** Très réactif, idéal pour un réglage rapide.
- **Mode 1 (Log) :** Sensation naturelle, fidèle à la version 3.4.4.
- **Commutateur :** Une macro `#define CurveOff345` est disponible dans `Audio.h` pour forcer l'utilisation de la courbe polynomiale originale de la v3.4.5 si nécessaire.
**Bénéfice :** Offre une montée en puissance immédiate et un ressenti auditif plus naturel, tout en permettant de revenir au comportement standard de la v3.4.5 via la macro.


### 4. Bypass DSP Dynamique
**Contexte :** La chaîne de filtres IIR (Égaliseur) consomme des cycles CPU pour chaque échantillon, même lorsque les gains sont neutres (0 dB).
**Amélioration :** Détection automatique de l'état neutre de l'égaliseur pour shunter intégralement la routine de calcul.
**Bénéfice :** Gain de 10 à 15% de cycles CPU lors de la lecture sans égalisation, prolongeant l'autonomie et libérant de la puissance pour d'autres tâches.


### 5. Fréquences EQ Dynamiques
**Contexte :** Les fréquences de coupure des 3 bandes de l'égaliseur (Basses, Médiums, Aigus) étaient auparavant figées dans le code de la bibliothèque.
**Amélioration :** Ajout des méthodes publiques `setToneFrequencies()` et `getToneFrequencies()` pour ajuster les fréquences de coupure au runtime.
**Bénéfice :** Permet d'ajuster dynamiquement la signature sonore pour s'adapter parfaitement au matériel audio utilisé.


### 6. Gestion EQ Anti-Clic
**Contexte :** Dans la bibliothèque originale, chaque modification de gain déclenchait une remise à zéro brutale des filtres, provoquant un "clic" ou un "glitch" sonore.
**Amélioration :** Refonte complète de `IIR_calculateCoefficients()` avec un système de suivi d'état (`m_last_eq`) et suppression de la réinitialisation systématique de la mémoire des filtres.
**Bénéfice :** Transition sonore parfaitement fluide lors de la manipulation de l'encodeur, offrant un ressenti professionnel sans aucune interruption de l'onde audio.


### 7. Stratégie Headroom EQ
**Contexte :** L'augmentation des gains de l'égaliseur peut saturer le signal (clipping). La bibliothèque originale réduit systématiquement le volume global pour compenser chaque boost.
**Amélioration :** Ajout de la macro `AUDIO_EQ_HEADROOM_MODE` pour choisir la sévérité de cette compensation.
- **Mode 0 (Totale) :** Protection maximale (volume réduit à 100% du boost).
- **Mode 1 (Hybride) :** Compromis (volume réduit à 50% du boost).
- **Mode 2 (Désactivée) :** Puissance maximale (aucune réduction automatique).
**Bénéfice :** Offre une flexibilité totale entre protection audio et puissance sonore brute.


### 8. Optimisations Performance & Mémoire
**Amélioration :** Regroupe plusieurs optimisations de bas niveau pour maximiser l'efficacité de l'ESP32.
- **Optimisation FPU :** Usage forcé de l'unité de calcul flottante matérielle 32 bits.
- **Constexpr :** Déplacement des tables de recherche internes en Flash (PROGMEM).
- **Buffer Persistant :** Utilisation d'un buffer `ps_ptr` persistant pour le ré-échantillonnage 48kHz.
- **Suppression STL :** Remplacement des derniers `std::string` par des chaînes C.
**Bénéfice :** Gain de performance global, réduction du stress sur l'allocateur mémoire et contrôle total de la signature sonore DSP.


> ℹ️ **Pour le détail de l'implémentation technique de ces fonctions, veuillez consulter le fichier [technic.md](./technic.md).**