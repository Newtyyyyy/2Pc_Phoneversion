#include <jni.h>
#include <string>
#include <mutex>
#include <atomic>
#include <opencv2/opencv.hpp>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <libusb.h>          // doit venir AVANT libuvc.h : definit LIBUSB_API_VERSION,
#include <libuvc/libuvc.h>   // qui conditionne la declaration de uvc_wrap (ouverture par fd)

// Macros pour afficher les logs dans la console d'Android Studio (Logcat)
#define LOG_TAG "NewtyVision_C++"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace cv;

// --- VARIABLES GLOBALES ---
static uvc_context_t*        uvcContext   = nullptr;
static uvc_device_handle_t*  uvcHandle    = nullptr;

// --- SEUILS DE DETECTION COULEUR (convention OpenCV : Teinte 0-179, Sat/Val 0-255) ---
// Modifiables en direct depuis Kotlin via setHsvRange(). Proteges par un mutex car
// lus par le thread libuvc et ecrits par le thread UI.
static std::mutex hsvMutex;
static Scalar HSV_LOW (130,  55,  75); // contour violet/magenta (chams), tolerant a la distance
static Scalar HSV_HIGH(168, 255, 255);

// Rayon de regroupement des clusters (0 = pixels colles seulement, plus grand = fusion large)
static std::atomic<int> clusterRadius{10};

// Cible : ecart X,Y (en pixels du crop) entre le centre de l'ecran et le centre
// de la plus grosse box. Lus par Kotlin pour l'affichage (et bientot l'envoi serie).
static std::atomic<int>  targetX{0};
static std::atomic<int>  targetY{0};
static std::atomic<bool> targetFound{false};
static std::atomic<int>  frameSeq{0}; // incremente a chaque trame traitee

// Surface d'affichage (protegee : posee depuis l'UI, utilisee depuis le thread libuvc)
static ANativeWindow*        nativeWindow = nullptr;
static std::mutex            windowMutex;
static int                   bufW = 0, bufH = 0; // geometrie courante du buffer

// ==========================================
// AFFICHAGE : copie une image RGBA vers la Surface Android
// ==========================================
static void afficherSurSurface(const Mat& rgba) {
    std::lock_guard<std::mutex> lock(windowMutex);
    if (nativeWindow == nullptr) {
        return;
    }

    // (Re)configure la geometrie du buffer si la taille de l'image change
    if (rgba.cols != bufW || rgba.rows != bufH) {
        ANativeWindow_setBuffersGeometry(nativeWindow, rgba.cols, rgba.rows, WINDOW_FORMAT_RGBA_8888);
        bufW = rgba.cols;
        bufH = rgba.rows;
    }

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(nativeWindow, &buffer, nullptr) != 0) {
        return;
    }

    // Copie ligne par ligne en respectant le stride du buffer Android
    auto* dst = static_cast<uint8_t*>(buffer.bits);
    int rows = std::min(rgba.rows, buffer.height);
    int rowBytes = std::min(rgba.cols, buffer.width) * 4;
    for (int y = 0; y < rows; ++y) {
        memcpy(dst + static_cast<size_t>(y) * buffer.stride * 4,
               rgba.ptr(y), rowBytes);
    }

    ANativeWindow_unlockAndPost(nativeWindow);
}

// ==========================================
// CALLBACK : appele par libuvc a chaque trame recue (sur son propre thread)
// ==========================================
static void frameCallback(uvc_frame_t* frame, void* /*userPtr*/) {
    if (frame == nullptr || frame->data == nullptr || frame->data_bytes == 0) {
        return;
    }

    // Buffers REUTILISES entre les trames : OpenCV ne realloue que si la taille change.
    // C'est la cle pour la latence (zero allocation memoire par trame).
    static Mat frameBrute, hsv, mask, gray, resultat, couleurRGBA;

    if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG) {
        // Flux compresse : on decode le JPEG avec OpenCV (libjpeg-turbo)
        Mat jpeg(1, static_cast<int>(frame->data_bytes), CV_8UC1, frame->data);
        frameBrute = imdecode(jpeg, IMREAD_COLOR);
    } else if (frame->frame_format == UVC_FRAME_FORMAT_YUYV) {
        // Flux non compresse YUY2 : conversion directe vers BGR
        Mat yuyv(frame->height, frame->width, CV_8UC2, frame->data);
        cvtColor(yuyv, frameBrute, COLOR_YUV2BGR_YUYV);
    } else {
        LOGE("Format de trame non gere : %d", frame->frame_format);
        return;
    }

    if (frameBrute.empty()) {
        LOGE("Erreur : trame vide apres decodage.");
        return;
    }

    // --- CROP CENTRAL FIXE : 10% de la largeur et de la hauteur, centre ---
    int cw = std::max(16, frameBrute.cols * 10 / 100);
    int ch = std::max(16, frameBrute.rows * 10 / 100);
    Rect crop((frameBrute.cols - cw) / 2, (frameBrute.rows - ch) / 2, cw, ch);
    Mat roiBGR = frameBrute(crop); // vue, sans copie

    // On lit les seuils courants (copie rapide sous verrou)
    Scalar bas, haut;
    {
        std::lock_guard<std::mutex> lock(hsvMutex);
        bas = HSV_LOW;
        haut = HSV_HIGH;
    }

    // --- DETECTION COULEUR (effet "color splash") sur le CROP uniquement ---
    // 1. Masque : 255 la ou le pixel est dans ta plage HSV, 0 ailleurs
    cvtColor(roiBGR, hsv, COLOR_BGR2HSV);
    inRange(hsv, bas, haut, mask);

    // 1b. Nettoyage anti-parasites : ouverture (enleve les pixels isoles)
    static const Mat noyauOpen = getStructuringElement(MORPH_ELLIPSE, Size(3, 3));
    morphologyEx(mask, mask, MORPH_OPEN, noyauOpen);
    // 1c. Fermeture : bouche les trous du contour causes par les ombres/reflets
    // -> l'ennemi reste UNE seule boite au lieu de se fragmenter.
    static const Mat noyauClose = getStructuringElement(MORPH_ELLIPSE, Size(7, 7));
    morphologyEx(mask, mask, MORPH_CLOSE, noyauClose);

    // 2. Fond en niveaux de gris (format RGBA pour la Surface)
    cvtColor(roiBGR, gray, COLOR_BGR2GRAY);
    cvtColor(gray, resultat, COLOR_GRAY2RGBA);

    // 3. On reintroduit la couleur d'origine la ou le masque est actif
    cvtColor(roiBGR, couleurRGBA, COLOR_BGR2RGBA);
    couleurRGBA.copyTo(resultat, mask);

    // 4. Regroupement en clusters + une boite verte par cluster
    int r = clusterRadius.load();
    static Mat masqueClusters;
    Mat* source = &mask;
    if (r > 0) {
        Mat noyau = getStructuringElement(MORPH_RECT, Size(2 * r + 1, 2 * r + 1));
        dilate(mask, masqueClusters, noyau);
        source = &masqueClusters;
    }

    static std::vector<std::vector<Point>> contours;
    contours.clear();
    findContours(*source, contours, RETR_EXTERNAL, CHAIN_APPROX_SIMPLE);

    // Collecte des boites de clusters valides
    Rect limites(0, 0, resultat.cols, resultat.rows);
    static std::vector<Rect> boites;
    boites.clear();
    for (auto& c : contours) {
        if (contourArea(c) < 15.0) continue; // anti-bruit (zone petite -> seuil bas)
        Rect boite = boundingRect(c);
        if (r > 0) {
            boite.x += r; boite.y += r;
            boite.width -= 2 * r; boite.height -= 2 * r;
        }
        boite &= limites;
        if (boite.width > 2 && boite.height > 2) {
            boites.push_back(boite);
            rectangle(resultat, boite, Scalar(0, 255, 0, 255), 2); // clusters : vert fin
        }
    }

    // 5. Cible = milieu du BORD SUPERIEUR de la box.
    // Critere : la plus proche du viseur (plus petit deplacement). Hysteresis : on
    // reste colle a la cible precedente tant qu'une box reste proche de sa derniere
    // position -> evite de sauter de cible quand les ombres bougent.
    Point centreEcran(resultat.cols / 2, resultat.rows / 2);
    static Point derniereCible;
    static bool  avaitCible = false;
    const double STICK2 = 22.0 * 22.0; // rayon de "collage" au carre (px)

    auto sommet = [](const Rect& b) { return Point(b.x + b.width / 2, b.y); };
    auto dist2  = [](Point a, Point b) {
        int dx = a.x - b.x, dy = a.y - b.y; return double(dx * dx + dy * dy);
    };

    int choisi = -1;
    double best = 1e18;
    if (avaitCible) { // priorite a la continuite (meme cible qu'avant)
        for (size_t i = 0; i < boites.size(); ++i) {
            double d = dist2(sommet(boites[i]), derniereCible);
            if (d < best) { best = d; choisi = static_cast<int>(i); }
        }
        if (best > STICK2) choisi = -1; // trop loin -> on relache
    }
    if (choisi < 0) { // sinon : la plus proche du viseur
        best = 1e18;
        for (size_t i = 0; i < boites.size(); ++i) {
            double d = dist2(sommet(boites[i]), centreEcran);
            if (d < best) { best = d; choisi = static_cast<int>(i); }
        }
    }

    if (choisi >= 0) {
        Rect b = boites[choisi];
        Point cible = sommet(b);
        targetX.store(cible.x - centreEcran.x);   // + = cible a droite
        targetY.store(cible.y - centreEcran.y);   // + = cible en bas
        targetFound.store(true);
        derniereCible = cible; avaitCible = true;

        rectangle(resultat, b, Scalar(255, 255, 0, 255), 2);            // cible : jaune
        line(resultat, b.tl(), Point(b.x + b.width, b.y), Scalar(0, 0, 255, 255), 2); // bord haut rouge
        circle(resultat, cible, 3, Scalar(255, 255, 0, 255), -1);       // point vise
        line(resultat, centreEcran, cible, Scalar(0, 255, 255, 255), 1);// ecran -> cible
    } else {
        targetFound.store(false);
        avaitCible = false;
    }

    frameSeq.fetch_add(1); // nouvelle trame traitee (pour l'envoi serie a la bonne cadence)

    // 6. Affichage (l'image garde son ratio 16:9 ; la SurfaceView est letterboxee)
    afficherSurSurface(resultat);
}

// ==========================================
// DIAGNOSTIC : liste TOUS les modes supportes par la carte (resolution @ fps)
// A regarder dans Logcat (tag NewtyVision_C++) pour connaitre le vrai plafond fps.
// ==========================================
static void listerFormats() {
    const uvc_format_desc_t* fmt = uvc_get_format_descs(uvcHandle);
    for (; fmt != nullptr; fmt = fmt->next) {
        const char* nomFmt =
                (fmt->bDescriptorSubtype == UVC_VS_FORMAT_MJPEG) ? "MJPEG" :
                (fmt->bDescriptorSubtype == UVC_VS_FORMAT_UNCOMPRESSED) ? "YUYV/non-compresse" : "autre";
        for (const uvc_frame_desc_t* fr = fmt->frame_descs; fr != nullptr; fr = fr->next) {
            // Les intervalles sont en unites de 100 ns -> fps = 10 000 000 / intervalle
            if (fr->intervals) {
                for (const uint32_t* it = fr->intervals; *it; ++it) {
                    int fps = static_cast<int>(10000000.0 / *it + 0.5);
                    LOGI("MODE dispo : %s %dx%d @ %d fps", nomFmt, fr->wWidth, fr->wHeight, fps);
                }
            } else {
                int fpsMin = static_cast<int>(10000000.0 / fr->dwMaxFrameInterval + 0.5);
                int fpsMax = static_cast<int>(10000000.0 / fr->dwMinFrameInterval + 0.5);
                LOGI("MODE dispo : %s %dx%d @ %d-%d fps", nomFmt, fr->wWidth, fr->wHeight, fpsMin, fpsMax);
            }
        }
    }
}

// ==========================================
// NEGOCIATION DE FORMAT (avec repli automatique)
// On essaie MJPEG (haute cadence) puis YUYV, a plusieurs resolutions.
// ==========================================
static bool negocierFormat(uvc_stream_ctrl_t* ctrl) {
    struct Essai { uvc_frame_format fmt; int w; int h; int fps; const char* nom; };
    // On PRIVILEGIE le 720p60 : 60 fps (plafond de la carte) + decodage 2x plus leger
    // que le 1080p. La carte ne depasse jamais 60 fps.
    const Essai essais[] = {
            { UVC_FRAME_FORMAT_MJPEG, 1280,  720, 60, "MJPEG 720p60"  },
            { UVC_FRAME_FORMAT_MJPEG, 1024,  768, 60, "MJPEG 1024x768@60" },
            { UVC_FRAME_FORMAT_MJPEG,  640,  480, 60, "MJPEG 640x480@60"  },
            { UVC_FRAME_FORMAT_MJPEG, 1280,  720, 30, "MJPEG 720p30"  },
            { UVC_FRAME_FORMAT_MJPEG, 1920, 1080, 60, "MJPEG 1080p60" },
            { UVC_FRAME_FORMAT_MJPEG, 1920, 1080, 30, "MJPEG 1080p30" },
            { UVC_FRAME_FORMAT_YUYV,   640,  480, 60, "YUYV 640x480@60" },
    };

    for (const auto& e : essais) {
        uvc_error_t res = uvc_get_stream_ctrl_format_size(uvcHandle, ctrl, e.fmt, e.w, e.h, e.fps);
        if (res >= 0) {
            LOGI("Format retenu : %s", e.nom);
            return true;
        }
    }
    LOGE("Aucun format supporte trouve sur la carte.");
    return false;
}

// ==========================================
// 1. DEMARRAGE DE LA CAMERA UVC via le file descriptor Android
// ==========================================
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_newtyvision_MainActivity_startCamera(JNIEnv* /*env*/, jobject /*this*/, jint fileDescriptor) {

    // CRUCIAL sur Android non-roote : libusb ne peut PAS scanner /dev/bus/usb ni /sys
    // (bloque par SELinux -> "uvc_init a echoue : I/O error"). On desactive l'enumeration :
    // le peripherique sera ouvert uniquement via le file descriptor donne par Android.
    // C'est la meme technique que les apps type "USB Camera".
    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);

    uvc_error_t res = uvc_init(&uvcContext, nullptr);
    if (res < 0) {
        LOGE("uvc_init a echoue : %s", uvc_strerror(res));
        return false;
    }

    // Ouverture du peripherique deja autorise par Android, via son file descriptor.
    // uvc_wrap est le chemin specifique Android (pas d'enumeration /dev/bus/usb).
    res = uvc_wrap(fileDescriptor, uvcContext, &uvcHandle);
    if (res < 0) {
        LOGE("uvc_wrap a echoue : %s", uvc_strerror(res));
        uvc_exit(uvcContext);
        uvcContext = nullptr;
        return false;
    }

    // On liste d'abord tous les modes que la carte sait faire (visible dans Logcat)
    listerFormats();

    // Negociation du meilleur format disponible (MJPEG de preference, sinon YUYV)
    uvc_stream_ctrl_t ctrl;
    if (!negocierFormat(&ctrl)) {
        uvc_close(uvcHandle);
        uvcHandle = nullptr;
        uvc_exit(uvcContext);
        uvcContext = nullptr;
        return false;
    }

    // Demarrage du streaming : libuvc appellera frameCallback sur son thread interne
    res = uvc_start_streaming(uvcHandle, &ctrl, frameCallback, nullptr, 0);
    if (res < 0) {
        LOGE("uvc_start_streaming a echoue : %s", uvc_strerror(res));
        uvc_close(uvcHandle);
        uvcHandle = nullptr;
        uvc_exit(uvcContext);
        uvcContext = nullptr;
        return false;
    }

    LOGI("Streaming UVC demarre. Detection couleur HSV active.");
    return true;
}

// ==========================================
// 2. ARRET ET NETTOYAGE MEMOIRE
// ==========================================
extern "C" JNIEXPORT void JNICALL
Java_com_example_newtyvision_MainActivity_stopCamera(JNIEnv* /*env*/, jobject /*this*/) {
    if (uvcHandle != nullptr) {
        uvc_stop_streaming(uvcHandle); // bloque jusqu'a l'arret du thread de callback
        uvc_close(uvcHandle);
        uvcHandle = nullptr;
    }
    if (uvcContext != nullptr) {
        uvc_exit(uvcContext);
        uvcContext = nullptr;
    }
    LOGI("Flux UVC libere proprement.");
}

// ==========================================
// 3. GESTION DE LA SURFACE D'AFFICHAGE
// ==========================================
extern "C" JNIEXPORT void JNICALL
Java_com_example_newtyvision_MainActivity_setSurface(JNIEnv* env, jobject /*this*/, jobject surface) {
    std::lock_guard<std::mutex> lock(windowMutex);
    if (nativeWindow != nullptr) {
        ANativeWindow_release(nativeWindow);
        nativeWindow = nullptr;
    }
    bufW = 0;
    bufH = 0;
    if (surface != nullptr) {
        nativeWindow = ANativeWindow_fromSurface(env, surface);
    }
}

// ==========================================
// 4. REGLAGE EN DIRECT DES SEUILS HSV
// ==========================================
extern "C" JNIEXPORT void JNICALL
Java_com_example_newtyvision_MainActivity_setHsvRange(
        JNIEnv* /*env*/, jobject /*this*/,
        jint hLow, jint sLow, jint vLow, jint hHigh, jint sHigh, jint vHigh) {
    std::lock_guard<std::mutex> lock(hsvMutex);
    HSV_LOW  = Scalar(hLow,  sLow,  vLow);
    HSV_HIGH = Scalar(hHigh, sHigh, vHigh);
}

// Distance de regroupement des clusters (0 = tres proche, 100 = tres eloigne)
extern "C" JNIEXPORT void JNICALL
Java_com_example_newtyvision_MainActivity_setClusterDistance(
        JNIEnv* /*env*/, jobject /*this*/, jint distance) {
    if (distance < 0) distance = 0;
    if (distance > 100) distance = 100;
    clusterRadius.store(distance);
}

// Renvoie [trouvee(0/1), X, Y, seq] : ecart en pixels du crop + numero de trame
extern "C" JNIEXPORT jintArray JNICALL
Java_com_example_newtyvision_MainActivity_getTarget(JNIEnv* env, jobject /*this*/) {
    jint vals[4] = { targetFound.load() ? 1 : 0, targetX.load(), targetY.load(), frameSeq.load() };
    jintArray arr = env->NewIntArray(4);
    env->SetIntArrayRegion(arr, 0, 4, vals);
    return arr;
}

// ==========================================
// 5. TEST DE LIAISON
// ==========================================
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_newtyvision_MainActivity_stringFromJNI(JNIEnv* env, jobject /*this*/) {
    std::string hello = "Moteur C++ NewtyVision Connecte !";
    return env->NewStringUTF(hello.c_str());
}
