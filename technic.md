# Technical Documentation : ESP32-audioI2S v3.4.5d_SLD

### 1. Modular Configuration

**SLD Optimization:** Selective inclusion via macros. Metadata structures and constant tables are wrapped in `#ifdef` blocks, ensuring that unused segments are physically excluded from the binary.
**Objective:** Drastic reduction of Flash footprint through conditional compilation.

1) Codec Selection

#### 📊 Flash Resource Savings (Measured)
**Codec OPUS**              **+170.7 KB** 
**Codec VORBIS**            **+108.6 KB**
**Codec AAC**               **+107.1 KB** 
**Codec MP3**               **+66.0 KB** 
**Codec FLAC**              **+32.2 KB** 
**Codec M4A (Container)**   **+14.6 KB** 
**FS System**               **+3.3 KB** 
**Codec WAV**               **+2.9 KB**

**Containers (OGG/M4A):** These formats are "wrappers". For instance, `AUDIO_CODEC_M4A` provides the logic to parse the MPEG-4 structure, while `AUDIO_CODEC_AAC` is the actual mathematical engine required to decode the sound.
*   **Dependency Logic:** Enabling a container automatically triggers its respective decoder dependency.
    ```cpp
    #ifdef AUDIO_CODEC_OGG
        #define AUDIO_CODEC_OPUS // OGG pack includes Opus by default
    #endif

    #ifdef AUDIO_CODEC_M4A
        #ifndef AUDIO_CODEC_AAC
            #define AUDIO_CODEC_AAC // M4A requires AAC engine
        #endif
    #endif
    ```

*   **M4A Details:** The `AUDIO_CODEC_M4A` macro activates only the "parser" capable of navigating "atoms" (data blocks) within the MPEG-4 structure. Without the `AUDIO_CODEC_AAC` engine, the extracted audio data could not be converted into a sound signal. Enabling the container thus forces its engine.

2) Granular Log Control via `AUDIO_LOG_LEVEL`. Saves 10 to 15 KB of Flash.

3) VU-Meter Management via `AUDIO_ENABLE_VUMETER`. Saves ~3-5% CPU (shunts DSP analysis).

4) Headroom EQ Dynamique via `AUDIO_EQ_HEADROOM_MODE`.
*   **Mode 0 (Full):** `m_audio_items.pre_gain = powf(10.0f, -total_boost_db / 20.0f);` (1:1 Reduction).
*   **Mode 1 (Hybrid):** `m_audio_items.pre_gain = powf(10.0f, -total_boost_db / 40.0f);` (0.5:1 Reduction).
*   **Mode 2 (Disabled):** `m_audio_items.pre_gain = 1.0f;` (Max Power).

5) Volume Ramp Speed via `AUDIO_VOLUME_RAMP_STEPS` (Default 50, SLD 25).

6) Modular File System Support (FS) via `AUDIO_ENABLE_FS`.

7) Modular TTS and String Class removal via `AUDIO_ENABLE_SPEECH`.


### 2. Robust I2S State Management
**Objective:** Prevent driver errors (`E (10013)`) during rapid zapping.

*   **Standard Behavior (Original v3.4.5):**
    The software flag is toggled independently of the hardware call result:
    ```cpp
    m_f_i2s_channel_enabled = false;
    return i2s_channel_disable(m_i2s_tx_handle);
    ```
    This creates a discrepancy if the driver takes time to respond, causing "channel not enabled" errors in subsequent calls.

*   **SLD Optimization (Hardware-Truth Synchronization):** 
    The software flag becomes an exact reflection of hardware reality:
    ```cpp
    esp_err_t Audio::I2Sstop() {
        if (!m_f_i2s_channel_enabled) return ESP_OK; // "Gatekeeper" protection
        esp_err_t err = i2s_channel_disable(m_i2s_tx_handle);
        if (err == ESP_OK) {
            m_f_i2s_channel_enabled = false; // Updated ONLY on hardware success
        }
        return err;
    }
    ```
*   **Impact:** Complete elimination of `E (10013)` log spam and secured rapid zapping (no more I2S bus crashes or freezes).


### 3. Restored "v3.4.4" Volume Curves
**Objective:** Restore the natural auditory feeling of v3.4.4 while maintaining v3.4.5 modern structure.

*   **Flexibility via `CurveOff345`:** The introduction of this macro in `Audio.h` allows choosing the algorithm generation:
    *   **Enabled (`#define`):** Standard v3.4.5 behavior (Unique cubic polynomial curve).
    *   **Disabled (SLD Default):** SLD optimization that restores the `setVolume(vol, curve)` parameter and the v3.4.4 dual-curve logic:
        *   **Mode 0 (Square):** `return t * t;` (Immediate power feeling).
        *   **Mode 1 (Log):** `v = vol * (exp((vol-1) * log(steps) / (steps-1)) / steps) / steps` (Surgical precision on lower volume steps).

*   **Implemented Code:**
    ```cpp
    #ifdef CurveOff345
        // Original 3.4.5 Polynomial curve
        float dB = -112.0f * t * t * t + 172.0f * t * t + MIN_DB;
        return powf(10.0f, dB / 20.0f);
    #else
        if (m_audio_items.volume_curve == 0) {
            return t * t; // Mode 0: Square
        } else {
            // Mode 1: Logarithmic (v3.4.4 formula)
            return (float)(volume * (exp((volume - 1.0f) * log((float)steps) / (float)(steps - 1)) / (float)steps) / (float)steps);
        }
    #endif
    ```


### 4. Dynamic DSP Bypass (Equalizer)
**Objective:** Eliminate CPU load for IIR filtering when the equalizer is at neutral (0 dB).

*   **Standard Behavior:** Even if gains are at 0 dB, the audio signal mathematically traverses the 3 Biquad filters. The processor performs thousands of multiplications per second for a result identical to the input (multiplication by 1.0).

*   **SLD Optimization:** Added a shortcut (Bypass) at the beginning of the `IIR_filter()` function.
    *   **Variables used:** `m_audio_items.gain_ls_db`, `m_audio_items.gain_peq_db`, `m_audio_items.gain_hs_db`.
    *   **Logic:** If the absolute sum of gains is zero, the function returns immediately, leaving the signal intact without any calculations.

*   **Code Implémenté :**
    ```cpp
    void Audio::IIR_filter(int32_t *sample) {
        // Vacuity test: if no gain is applied, exit immediately
        if (m_audio_items.gain_ls_db == 0.0f && m_audio_items.gain_peq_db == 0.0f && m_audio_items.gain_hs_db == 0.0f) {
            return;
        }
        // ... IIR calculations ...
    }
    ```
*   **Impact:** 10 to 15% CPU resource gain during standard playback, improving WebRadio stream stability.


### 5. Dynamic EQ Frequencies
**Objective:** Enable full customization of the equalizer's frequency response without recompiling code.

*   **SLD Optimization:** Added two public methods to the `Audio` class to manipulate the cutoff points of the Low-Shelf (Bass), Peaking-EQ (Mid), and High-Shelf (Treble) filters.

*   **Implemented Code:**
    ```cpp
    void Audio::setToneFrequencies(uint16_t freqLS, uint16_t freqPEQ, uint16_t freqHS) {
        m_audio_items.freq_ls_Hz = freqLS;
        m_audio_items.freq_peq_Hz = freqPEQ;
        m_audio_items.freq_hs_Hz = freqHS;
        IIR_calculateCoefficients(); // Immediate recalculation
    }

    void Audio::getToneFrequencies(uint16_t *freqLS, uint16_t *freqPEQ, uint16_t *freqHS) {
        if (freqLS)  *freqLS  = m_audio_items.freq_ls_Hz;
        if (freqPEQ) *freqPEQ = m_audio_items.freq_peq_Hz;
        if (freqHS)  *freqHS  = m_audio_items.freq_hs_Hz;
    }
    ```


### 6. Anti-Click EQ Management
**Objective:** Remove audio artifacts (clicks/glitchs) during equalizer adjustments.

*   **Standard Behavior:** The default implementation performs a `memset(state, 0)` on the Biquad filter history upon each update. 
Cette purge de la "mémoire" du filtre provoque un saut brutal de l'onde sonore, créant une rupture acoustique désagréable.

*   **SLD Optimization:** 
*   **Suivi d'État :** Introduction d'une structure privée `m_last_eq` mémorisant les derniers réglages (gains, fréquences et sample rate).

*   **Code Implémenté :**
    ```cpp
    struct {
        float gain_ls, gain_peq, gain_hs;
        uint16_t freq_ls, freq_peq, freq_hs;
        uint32_t sampleRate;
    } m_last_eq;
    ```
    *   **Mise à jour Sélective :** Seule la bande (Bass, Mid, or Treble) that actually changed is recalculated.
    *   **Transition Fluide :** Suppression du `memset` global. En préservant l'historique des échantillons dans les filtres, l'onde audio reste continue lors de la transition des coefficients, garantissant un réglage parfaitement inaudible pour l'oreille.


### 7. Dynamic EQ Headroom
**Objective:** Provide a choice between strict audio protection and maximum sound dynamics.

*   **Standard Behavior (Mode 0):** The library systematically reduces global gain (`pre_gain`) by the same amount as the highest EQ boost to avoid any risk of saturation (clipping).

*   **SLD Optimization:** Introduced the `AUDIO_EQ_HEADROOM_MODE` macro to dose this volume penalty. Since the ESP32 processes audio in 32 bits, some headroom naturally exists before actual saturation.

*   **Implemented Codes:**
    ```cpp
    #if AUDIO_EQ_HEADROOM_MODE == 0
        // Full Protection: volume reduced by 1 dB for every 1 dB boost
        m_audio_items.pre_gain = powf(10.0f, -total_boost_db / 20.0f);
    #elif AUDIO_EQ_HEADROOM_MODE == 1
        // Hybrid Mode: reduction of only 0.5 dB for every 1 dB boost
        m_audio_items.pre_gain = powf(10.0f, -total_boost_db / 40.0f);
    #else
        // Disabled Mode: no reduction, maximum power delivered
        m_audio_items.pre_gain = 1.0f;
    #endif
    ```
*   **Impact:** Mode 2 allows the equalizer to behave like high-fidelity hardware, without global volume drops when boosting bass.


### 8. Performance & Memory Optimizations
**Objective:** Maximize CPU efficiency, reduce fragmentation, and stabilize memory usage.

*   **Hardware FPU Optimization:**
    *   **Action:** Replaced `double` literals (e.g., `10.0`) with `float` literals (`10.0f`) and standard math functions with their `float` versions (`powf`, `log10f`).
    *   **Impact:** Crucial for **ESP32** and **S3**, which only have a 32-bit FPU. This avoids very slow 64-bit software emulation.

*   **Static Memory Management (Constexpr):**
    *   **Action:** Migrated internal lookup tables and static strings to Flash memory (**PROGMEM**) via `constexpr`.
    *   **Impact:** Frees up precious RAM for streaming and buffers.

*   **Persistent Resample Buffer:**
    *   **Action:** Used a single persistent `ps_ptr` buffer for 48kHz resampling (Bluetooth ready).
    *   **Impact:** Removes repetitive allocations/deallocations during each cycle, eliminating a major source of heap fragmentation.

*   **STL Removal:**
    *   **Action:** Replaced `std::string` with traditional C strings in critical paths.
    *   **Impact:** Reduces memory footprint and improves system predictability during long listening sessions.

***

# Documentation Technique : ESP32-audioI2S v3.4.5d_SLD


### 1. Configuration Modulaire

**Optimisation SLD :** Inclusion sélective via macros. Les structures de métadonnées et les tables de constantes sont encadrées par des blocs `#ifdef`, garantissant que les segments inutilisés sont physiquement exclus du binaire.
**Objectif :** Réduction drastique de l'empreinte Flash via la compilation conditionnelle.

1) Sélection des Codecs

#### 📊 Économies de Ressources Flash (Mesurées)
**Codec OPUS**              **+170.7 Ko** 
**Codec VORBIS**            **+108.6 Ko**
**Codec AAC**               **+107.1 Ko** 
**Codec MP3**               **+66.0 Ko** 
**Codec FLAC**              **+32.2 Ko** 
**Codec M4A (Conteneur)**   **+14.6 Ko** 
**Système FS**              **+3.3 Ko** 
**Codec WAV**               **+2.9 Ko**

**Conteneurs (OGG/M4A) :** Ces formats sont des "enveloppes". Par exemple, `AUDIO_CODEC_M4A` gère la lecture de la structure MPEG-4, tandis que `AUDIO_CODEC_AAC` est le moteur mathématique qui décode le son.
*   **Logique de Dépendance :** L'activation d'un conteneur déclenche automatiquement sa dépendance décodeur.
    ```cpp
    #ifdef AUDIO_CODEC_OGG
        #define AUDIO_CODEC_OPUS // Le pack OGG inclut Opus par défaut
    #endif

    #ifdef AUDIO_CODEC_M4A
        #ifndef AUDIO_CODEC_AAC
            #define AUDIO_CODEC_AAC // M4A nécessite le moteur AAC
        #endif
    #endif
    ```

*   **Détails M4A :** La macro `AUDIO_CODEC_M4A` active uniquement le "parser" capable de naviguer dans les "atomes" (blocs de données) de la structure MPEG-4. Sans le moteur `AUDIO_CODEC_AAC`, les données audio extraites ne pourraient pas être transformées en signal sonore. L'activation du conteneur force donc celle de son moteur.

2) Contrôle Granulaire des Logs via `AUDIO_LOG_LEVEL`. Gain de 10 à 15 Ko de Flash.

3) Gestion du VU-Mètre via `AUDIO_ENABLE_VUMETER`. Gain de ~3-5% CPU (shunt de l'analyse DSP).

4) Headroom EQ Dynamique via `AUDIO_EQ_HEADROOM_MODE`.
*   **Mode 0 (Totale) :** `m_audio_items.pre_gain = powf(10.0f, -total_boost_db / 20.0f);` (Réduction 1:1).
*   **Mode 1 (Hybride) :** `m_audio_items.pre_gain = powf(10.0f, -total_boost_db / 40.0f);` (Réduction 0.5:1).
*   **Mode 2 (Désactivée) :** `m_audio_items.pre_gain = 1.0f;` (Puissance Max).

5) Vitesse de Rampe du Volume via `AUDIO_VOLUME_RAMP_STEPS` (Défaut 50, SLD 25).

6) Modularité du Système de Fichiers (FS) via `AUDIO_ENABLE_FS`.

7) Modularité TTS et suppression de la classe String via `AUDIO_ENABLE_SPEECH`.


### 2. Gestion d'État I2S Robuste
**Objectif :** Éviter les erreurs driver (`E (10013)`) lors du zapping rapide.

*   **Comportement Standard (v3.4.5 d'origine) :**
    Le flag logiciel est basculé indépendamment du résultat de l'appel matériel :
    ```cpp
    m_f_i2s_channel_enabled = false;
    return i2s_channel_disable(m_i2s_tx_handle);
    ```
    Cela crée un décalage si le driver met du temps à répondre, provoquant des erreurs "channel not enabled" lors des appels suivants.

*   **Optimisation SLD (Synchronisation Hardware-Truth) :** 
    Le flag logiciel devient le reflet exact de la réalité matérielle :
    ```cpp
    esp_err_t Audio::I2Sstop() {
        if (!m_f_i2s_channel_enabled) return ESP_OK; // Protection "Gatekeeper"
        esp_err_t err = i2s_channel_disable(m_i2s_tx_handle);
        if (err == ESP_OK) {
            m_f_i2s_channel_enabled = false; // Mise à jour UNIQUEMENT sur succès matériel
        }
        return err;
    }
    ```
*   **Impact :** Élimination totale du spam de logs `E (10013)` et sécurisation du zapping rapide (plus de crash ou de blocage du bus I2S).


### 3. Restauration des Courbes de Volume "v3.4.4"
**Objectif :** Retrouver le ressenti auditif naturel de la v3.4.4 tout en conservant la structure moderne de la v3.4.5.

*   **Flexibilité via `CurveOff345` :** L'introduction de cette macro dans `Audio.h` permet de choisir sa génération d'algorithme de volume :
    *   **Activée (`#define`) :** Comportement Standard v3.4.5 (Courbe polynomiale cubique unique).
    *   **Désactivée (Par défaut SLD) :** Optimisation SLD qui rétablit le paramètre `setVolume(vol, curve)` et la dualité de courbes de la v3.4.4 :
        *   **Mode 0 (Square) :** `return t * t;` (Sensation de puissance immédiate).
        *   **Mode 1 (Log) :** `v = vol * (exp((vol-1) * log(steps) / (steps-1)) / steps) / steps` (Précision chirurgicale sur les premiers crans du volume).

*   **Code Implémenté :**
    ```cpp
    #ifdef CurveOff345
        // Original 3.4.5 Polynomial curve
        float dB = -112.0f * t * t * t + 172.0f * t * t + MIN_DB;
        return powf(10.0f, dB / 20.0f);
    #else
        if (m_audio_items.volume_curve == 0) {
            return t * t; // Mode 0: Square
        } else {
            // Mode 1: Logarithmic (v3.4.4 formula)
            return (float)(volume * (exp((volume - 1.0f) * log((float)steps) / (float)(steps - 1)) / (float)steps) / (float)steps);
        }
    #endif
    ```


### 4. Bypass DSP Dynamique (Égaliseur)
**Objectif :** Éliminer la charge CPU du filtrage IIR lorsque l'égaliseur est au neutre (0 dB).

*   **Comportement Standard :** Même si les gains sont à 0 dB, le signal audio traverse mathématiquement les 3 filtres Biquad. Le processeur effectue des milliers de multiplications par seconde pour un résultat identique à l'entrée (multiplication par 1.0).

*   **Optimisation SLD :** Ajout d'une condition de court-circuit (Bypass) à l'entrée de la fonction `IIR_filter()`. 
    *   **Variables utilisées :** `m_audio_items.gain_ls_db`, `m_audio_items.gain_peq_db`, `m_audio_items.gain_hs_db`.
    *   **Logique :** Si la somme absolue des gains est nulle, la fonction s'interrompt immédiatement (`return`), laissant le signal intact sans aucun calcul.

*   **Code Implémenté :**
    ```cpp
    void Audio::IIR_filter(int32_t *sample) {
        // Test de vacuité : si aucun gain n'est appliqué, on sort immédiatement
        if (m_audio_items.gain_ls_db == 0.0f && m_audio_items.gain_peq_db == 0.0f && m_audio_items.gain_hs_db == 0.0f) {
            return;
        }
        // ... calculs IIR ...
    }
    ```
*   **Impact :** Gain de 10 à 15% de ressources CPU lors de la lecture standard, améliorant la stabilité du flux WebRadio.


### 5. Fréquences EQ Dynamiques
**Objectif :** Permettre la personnalisation totale de la réponse fréquentielle de l'égaliseur sans recompiler le code.

*   **Optimisation SLD :** Ajout de deux méthodes publiques à la classe `Audio` pour manipuler les points de coupure des filtres Low-Shelf (Basses), Peaking-EQ (Médiums) et High-Shelf (Aigus).

*   **Code Implémenté :**
    ```cpp
    void Audio::setToneFrequencies(uint16_t freqLS, uint16_t freqPEQ, uint16_t freqHS) {
        m_audio_items.freq_ls_Hz = freqLS;
        m_audio_items.freq_peq_Hz = freqPEQ;
        m_audio_items.freq_hs_Hz = freqHS;
        IIR_calculateCoefficients(); // Recalcul immédiat
    }

    void Audio::getToneFrequencies(uint16_t *freqLS, uint16_t *freqPEQ, uint16_t *freqHS) {
        if (freqLS)  *freqLS  = m_audio_items.freq_ls_Hz;
        if (freqPEQ) *freqPEQ = m_audio_items.freq_peq_Hz;
        if (freqHS)  *freqHS  = m_audio_items.freq_hs_Hz;
    }
    ```


### 6. Gestion EQ Anti-Clic
**Objectif :** Supprimer les artefacts sonores (clics/glitchs) lors des réglages de l'égaliseur.

*   **Comportement Standard :** L'implémentation par défaut effectue un `memset(state, 0)` sur l'historique des filtres Biquad à chaque mise à jour. 
Cette purge de la "mémoire" du filtre provoque un saut brutal de l'onde sonore, créant une rupture acoustique désagréable.

*   **Optimisation SLD :** 
*   **Suivi d'État :** Introduction d'une structure privée `m_last_eq` mémorisant les derniers réglages (gains, fréquences et sample rate).

*   **Code Implémenté :**
    ```cpp
    struct {
        float gain_ls, gain_peq, gain_hs;
        uint16_t freq_ls, freq_peq, freq_hs;
        uint32_t sampleRate;
    } m_last_eq;
    ```
    *   **Mise à jour Sélective :** Seule la bande (Basses, Médiums ou Aigus) ayant réellement changé est recalculée.
    *   **Transition Fluide :** Suppression du `memset` global. En préservant l'historique des échantillons dans les filtres, l'onde audio reste continue lors de la transition des coefficients, garantissant un réglage parfaitement inaudible pour l'oreille.


### 7. Stratégie Headroom EQ
**Objectif :** Offrir le choix entre protection audio stricte et dynamique sonore maximale.

*   **Comportement Standard (Mode 0) :** La bibliothèque réduit systématiquement le gain global (`pre_gain`) du même montant que le boost EQ le plus élevé pour éviter tout risque de saturation (clipping).
*   **Optimisation SLD :** Introduction de la macro `AUDIO_EQ_HEADROOM_MODE` permettant de doser cette pénalité de volume. Comme l'ESP32 traite l'audio en 32 bits, une certaine marge de manœuvre (Headroom) existe naturellement avant la saturation réelle.

*   **Codes Implémentés :**
    ```cpp
    #if AUDIO_EQ_HEADROOM_MODE == 0
        // Protection Totale : volume réduit de 1 dB pour chaque 1 dB de boost
        m_audio_items.pre_gain = powf(10.0f, -total_boost_db / 20.0f);
    #elif AUDIO_EQ_HEADROOM_MODE == 1
        // Mode Hybride : réduction de seulement 0.5 dB pour chaque 1 dB de boost
        m_audio_items.pre_gain = powf(10.0f, -total_boost_db / 40.0f);
    #else
        // Mode Désactivé : aucune réduction, puissance maximale délivrée
        m_audio_items.pre_gain = 1.0f;
    #endif
    ```
*   **Impact :** Le Mode 2 permet à l'égaliseur de se comporter comme un matériel haute-fidélité, sans baisse de volume globale lors de l'augmentation des basses.


### 8. Optimisations Performance & Mémoire
**Objectif :** Maximiser l'efficacité du CPU, réduire la fragmentation et stabiliser l'usage mémoire.

*   **Optimisation FPU matérielle :**
    *   **Action :** Remplacement des littéraux `double` (ex: `10.0`) par des littéraux `float` (`10.0f`) et des fonctions mathématiques standard par leurs versions `float` (`powf`, `log10f`).
    *   **Impact :** Crucial pour **ESP32** et **S3** qui ne possèdent qu'une FPU 32 bits. Cela évite l'émulation logicielle 64 bits très lente.

*   **Gestion Mémoire Statique (Constexpr) :**
    *   **Action :** Migration des tables de recherche et des chaînes de caractères internes vers la Flash (**PROGMEM**) via `constexpr`.
    *   **Impact :** Libère de la RAM précieuse pour le streaming et les buffers.

*   **Buffer de Ré-échantillonnage Persistant :**
    *   **Action :** Utilisation d'un buffer `ps_ptr` unique et persistant pour le passage à 48kHz (Bluetooth ready).
    *   **Impact :** Supprime les allocations/désallocations répétitives à chaque cycle, éliminant une source majeure de fragmentation du tas (heap).

*   **Suppression de la STL :**
    *   **Action :** Remplacement des `std::string` par des chaînes C traditionnelles dans les chemins critiques.
    *   **Impact :** Réduit l'empreinte mémoire et améliore la prédictibilité du système lors des longues sessions d'écoute.

