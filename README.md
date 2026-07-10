# NewtyVision

Application Android qui capture en direct le flux d'une **carte d'acquisition HDMI→USB**
(puce MacroSilicon, VID `0x345F` / PID `0x2130`) et le traite en temps réel avec **OpenCV**
côté natif (C++), via **libuvc**.

L'image est affichée en plein écran avec un effet *color-splash* : tout passe en niveaux de
gris **sauf** les pixels dans une plage de couleur HSV réglable, et chaque groupe de pixels
détectés (cluster) est entouré d'une boîte.

---

## Fonctionnalités

- 🎥 Lecture d'une caméra **UVC externe** sur Android **sans root**, via le *file descriptor*
  fourni par `UsbManager` (`libusb_wrap_sys_device` / `uvc_wrap`).
- ⚡ Rendu direct sur une `SurfaceView` (`ANativeWindow`), sans copie JNI par image.
- 🎨 Détection de couleur **HSV** avec effet color-splash.
- 🟩 Regroupement en **clusters** + boîte englobante par cluster.
- 🎚️ **Réglages en direct** (sliders) : seuils HSV et distance de regroupement des clusters.
- 🔄 Négociation automatique du format (MJPEG puis YUYV, plusieurs résolutions).
- 📱 Plein écran immersif, orientation paysage.

---

## Architecture

```
Kotlin (MainActivity)                 C++ (native-lib.cpp)
─────────────────────                 ────────────────────
UsbManager  ── permission ──►  file descriptor
                                        │
                                        ▼
                                  libuvc (uvc_wrap)  ── flux UVC
                                        │
                                        ▼
                                  OpenCV : décodage MJPEG,
                                  HSV / inRange, clusters
                                        │
                                        ▼
Surface  ◄──────────────────  ANativeWindow (rendu RGBA)
```

- **`app/src/main/java/.../MainActivity.kt`** : permission USB, cycle de vie de la Surface,
  sliders de réglage, pont JNI.
- **`app/src/main/cpp/native-lib.cpp`** : ouverture UVC, décodage, traitement OpenCV, rendu.
- **`app/src/main/cpp/CMakeLists.txt`** : récupère **libusb** et **libuvc** (via `FetchContent`)
  et les lie à OpenCV.

---

## Prérequis

- **Android Studio** (AGP 9.x) et le **NDK** + **CMake** (installables depuis le SDK Manager).
- L'**OpenCV Android SDK** (testé avec 4.12) décompressé quelque part sur ton disque.
- Un appareil Android avec **USB Host / OTG** et une carte de capture UVC.
- Connexion internet au **premier build** (téléchargement de libusb + libuvc).

---

## Configuration

Le chemin vers OpenCV est défini dans
[`app/src/main/cpp/CMakeLists.txt`](app/src/main/cpp/CMakeLists.txt) :

```cmake
set(OpenCV_DIR "C:/Users/newty/Desktop/apkmobile/OpenCV-android-sdk/sdk/native/jni")
```

➡️ **Adapte cette ligne** au chemin de ton propre OpenCV Android SDK avant de compiler.

---

## Build & lancement

1. Ouvre le projet dans Android Studio.
2. Adapte le chemin `OpenCV_DIR` (voir ci-dessus).
3. Branche ton appareil, laisse Gradle synchroniser (le 1ᵉʳ build est long).
4. **Run ▶**.
5. Branche la carte de capture → autorise la popup USB → le flux s'affiche.

> Compilé pour l'ABI `arm64-v8a` uniquement (voir `abiFilters` dans
> [`app/build.gradle.kts`](app/build.gradle.kts)).

---

## Utilisation

- **Touche l'écran** pour afficher / masquer le panneau de réglages.
- **Sliders** : ajuste les seuils **Teinte / Saturation / Valeur** (bas et haut) et la
  **distance des clusters** (0 = taches collées uniquement, 100 = fusion large).
- Les pixels dans la plage restent en couleur ; le reste passe en gris ; chaque cluster est
  encadré de vert.

Convention OpenCV : **Teinte 0–179**, **Saturation / Valeur 0–255**.

---

## Dépannage

- **L'app crashe au branchement** → vérifie que le `PendingIntent` USB est *explicite*
  (`setPackage`) : obligatoire depuis Android 14.
- **Écran noir / `uvc_init a echoue : I/O error`** → SELinux bloque l'énumération USB ;
  `LIBUSB_OPTION_NO_DEVICE_DISCOVERY` doit être activé avant `uvc_init` (déjà en place).
- **Aucune couleur en surbrillance** → la plage HSV ne correspond pas ; ajuste les sliders.
  Le rouge est aux extrémités du cercle de teinte (≈0–10 et ≈170–179).
- **Logs natifs** : filtre `NewtyVision_C++` dans Logcat.
