#include <jni.h>
#include <string>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cmath>
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
static Scalar HSV_LOW (140, 120, 180); // valeurs PC (flux YUYV non-compresse = couleurs propres)
static Scalar HSV_HIGH(160, 200, 255);

// Rayon de regroupement des clusters (0 = pixels colles seulement, plus grand = fusion large)
static std::atomic<int> clusterRadius{10};

// Cible : ecart X,Y (en pixels du crop) entre le centre de l'ecran et le centre
// de la plus grosse box. Lus par Kotlin pour l'affichage (et bientot l'envoi serie).
static std::atomic<int>  targetX{0};
static std::atomic<int>  targetY{0};
static std::atomic<bool> targetFound{false};
static std::atomic<int>  frameSeq{0}; // incremente a chaque trame traitee
static std::atomic<int>  boxW{0};     // taille de la box cible (px du crop)
static std::atomic<int>  boxH{0};
static std::atomic<int>  centreH{0};  // HSV moyen au centre du viseur (pour la pipette)
static std::atomic<int>  centreS{0};
static std::atomic<int>  centreV{0};

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Reglages de suivi (modifiables en direct depuis Kotlin)
static std::atomic<int> aimOffsetX{0};   // decalage du point de consigne en X (px du crop)
static std::atomic<int> aimOffsetY{0};   // decalage du point de consigne en Y (px du crop)
static std::atomic<int> lissagePct{60};  // 0..100 : lissage EMA (100 = brut, bas = tres lisse)
static std::atomic<int> headBandPct{22}; // % du haut de la silhouette pris pour viser la "tete"
static std::atomic<int> stabilitePct{70};// (inutilise dans la version simple)
static std::atomic<int> minPixels{30};   // aire mini d'un blob pour etre une cible (anti-parasites)
static std::atomic<int> predictionMs{60};// avance de prediction (ms) pour compenser la latence
static std::atomic<int> cropPctX{28};    // % de largeur detectee (zone horizontale)
static std::atomic<int> cropPctY{16};    // % de hauteur detectee (zone verticale)

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
// FILTRE ONE EURO : lissage adaptatif (fort au repos = zero tremblement ; faible en
// mouvement rapide = zero lag). Standard pour le tracking de pointeur. Donne aussi la
// vitesse lissee (pour predire et compenser la latence).
// ==========================================
struct LowPass {
    double s = 0; bool init = false;
    double filter(double x, double a) {
        if (!init) { s = x; init = true; } else { s = a * x + (1.0 - a) * s; }
        return s;
    }
    void reset() { init = false; }
};
struct OneEuro {
    double mincutoff = 1.0, beta = 0.02, dcutoff = 1.0;
    LowPass xf, dxf; double xPrev = 0; bool init = false; double vel = 0;
    static double alpha(double cutoff, double dt) {
        double tau = 1.0 / (2.0 * CV_PI * cutoff);
        return 1.0 / (1.0 + tau / dt);
    }
    double filter(double x, double dt) {
        double dx = init ? (x - xPrev) / dt : 0.0;
        vel = dxf.filter(dx, alpha(dcutoff, dt));            // vitesse lissee (px/s)
        double cutoff = mincutoff + beta * std::fabs(vel);
        double fx = xf.filter(x, alpha(cutoff, dt));
        xPrev = x; init = true;
        return fx;
    }
    void reset() { xf.reset(); dxf.reset(); init = false; vel = 0; }
};

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
        // Flux non compresse YUY2 : conversion directe vers BGR.
        // GARDE-FOU : sous forte charge (640x480@60 en USB2) la carte envoie des trames
        // TRONQUEES -> sans ce controle, cvtColor lit hors memoire et crashe (SIGSEGV).
        size_t attendu = (size_t) frame->width * frame->height * 2;
        if (frame->data_bytes < attendu) {
            LOGE("trame YUYV tronquee (%zu < %zu) -> ignoree", frame->data_bytes, attendu);
            return;
        }
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

    // --- CROP CENTRAL REGLABLE : % de largeur (Zone X) et % de hauteur (Zone Y) ---
    int cw = std::max(16, frameBrute.cols * cropPctX.load() / 100);
    int ch = std::max(16, frameBrute.rows * cropPctY.load() / 100);
    if (cw > frameBrute.cols) cw = frameBrute.cols;
    if (ch > frameBrute.rows) ch = frameBrute.rows;
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

    // Collecte des blobs au-dessus du SEUIL DE PIXELS (anti-parasites reglable).
    Rect limites(0, 0, resultat.cols, resultat.rows);
    static std::vector<Rect> boites;
    boites.clear();
    int seuilPix = minPixels.load();
    for (auto& c : contours) {
        Rect boite = boundingRect(c);
        if (r > 0) {
            // On retire le rayon de dilatation pour retrouver la box reelle du blob.
            boite.x += r; boite.y += r;
            boite.width -= 2 * r; boite.height -= 2 * r;
        }
        boite &= limites;
        if (boite.width <= 1 || boite.height <= 1) continue;
        // Anti-parasites FIABLE : on compte les VRAIS pixels colores (masque d'origine)
        // dans la box, pas l'aire du contour DILATE. Sans ca, un blob de 3 px dilate
        // par le regroupement de clusters gonfle et passe le seuil a tort.
        if (countNonZero(mask(boite)) < seuilPix) continue; // ex : 3 px -> ignore
        boites.push_back(boite);
        rectangle(resultat, boite, Scalar(0, 255, 0, 255), 1); // blobs valides : vert fin
    }

    // 5. Cible = le blob valide le plus PROCHE DU VISEUR. Point vise = haut de la box
    //    (un peu descendu = tete) + Offset Y. Lissage EMA simple. Simple et efficace.
    Point centreEcran(resultat.cols / 2, resultat.rows / 2);
    static float lisseX = 0, lisseY = 0;
    static bool  lisseInit = false;

    auto pointTete = [](const Rect& b) {
        return Point(b.x + b.width / 2, b.y + b.height / 8); // haut-centre, ~tete
    };

    int choisi = -1;
    double best = 1e18;
    for (size_t i = 0; i < boites.size(); ++i) {
        Point p = pointTete(boites[i]);
        int dx = p.x - centreEcran.x, dy = p.y - centreEcran.y;
        double d = (double) dx * dx + (double) dy * dy;
        if (d < best) { best = d; choisi = static_cast<int>(i); }
    }

    if (choisi >= 0) {
        Rect b = boites[choisi];
        Point cible = pointTete(b);

        // Lissage EMA simple (slider Lissage : bas = tres lisse, haut = brut/reactif).
        float a = 0.08f + (lissagePct.load() / 100.0f) * 0.90f; // 0.08..0.98
        if (!lisseInit) { lisseX = cible.x; lisseY = cible.y; lisseInit = true; }
        else { lisseX = a * cible.x + (1 - a) * lisseX; lisseY = a * cible.y + (1 - a) * lisseY; }

        Point visee(cvRound(lisseX) + aimOffsetX.load(),
                    cvRound(lisseY) + aimOffsetY.load());
        double ox = visee.x - centreEcran.x; // offset actuel (px)
        double oy = visee.y - centreEcran.y;

        // --- PREDICTION anti-latence : on estime la vitesse de la cible et on vise ou
        // elle SERA dans "prediction" ms -> compense le delai telephone<->PC + traitement.
        static double prevOX = 0, prevOY = 0, velOX = 0, velOY = 0;
        static long long prevT = 0; static bool hasPrev = false;
        long long now = nowMs();
        if (hasPrev) {
            double dt = (double) (now - prevT); // ms
            if (dt > 1.0 && dt < 300.0) {
                double vx = (ox - prevOX) / dt, vy = (oy - prevOY) / dt; // px/ms
                velOX = 0.4 * vx + 0.6 * velOX; // vitesse lissee
                velOY = 0.4 * vy + 0.6 * velOY;
            }
        }
        prevOX = ox; prevOY = oy; prevT = now; hasPrev = true;
        double lead = (double) predictionMs.load();
        int px = (int) lround(ox + velOX * lead);
        int py = (int) lround(oy + velOY * lead);

        targetX.store(px);
        targetY.store(py);
        targetFound.store(true);

        Point viseePred(px + centreEcran.x, py + centreEcran.y);
        rectangle(resultat, b, Scalar(255, 255, 0, 255), 2);            // cible : jaune
        circle(resultat, visee, 3, Scalar(255, 0, 255, 255), -1);       // point mesure (magenta)
        circle(resultat, viseePred, 3, Scalar(0, 255, 0, 255), -1);     // point PREDIT (vert)
        line(resultat, centreEcran, viseePred, Scalar(0, 255, 255, 255), 1);
    } else {
        targetFound.store(false);
        lisseInit = false;
    }

    frameSeq.fetch_add(1); // nouvelle trame traitee (pour l'envoi serie a la bonne cadence)

    // --- PIPETTE : HSV moyen d'un petit carre au centre du viseur (aide au reglage) ---
    {
        int cx = hsv.cols / 2, cy = hsv.rows / 2, rr = 3;
        Rect c = Rect(cx - rr, cy - rr, 2 * rr + 1, 2 * rr + 1) & Rect(0, 0, hsv.cols, hsv.rows);
        if (c.width > 0 && c.height > 0) {
            Scalar m = mean(hsv(c));
            centreH.store((int)m[0]); centreS.store((int)m[1]); centreV.store((int)m[2]);
            char buf[64];
            snprintf(buf, sizeof(buf), "H%d S%d V%d", (int)m[0], (int)m[1], (int)m[2]);
            putText(resultat, buf, Point(2, 12), FONT_HERSHEY_SIMPLEX, 0.4,
                    Scalar(0, 255, 255, 255), 1);
            rectangle(resultat, c, Scalar(255, 255, 255, 255), 1); // ou on echantillonne
        }
    }

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
    // PRIORITE : NON-COMPRESSE (YUYV) 720p60 -> couleur PROPRE + 60 fps, ideal detection.
    // Possible uniquement en USB3 (la bande passante YUYV60 ne passe pas en USB2 -> trames
    // tronquees). Repli MJPEG 60 fps si on est en USB2 (couleur sale mais fluide).
    // La carte (MS2130) ne depasse jamais 60 fps.
    const Essai essais[] = {
            // USB3 : couleur propre + fluide (le graal pour la detection HSV)
            { UVC_FRAME_FORMAT_YUYV,  1280,  720, 60, "YUYV 720p60 (non-compresse)"  },
            { UVC_FRAME_FORMAT_YUYV,  1920, 1080, 60, "YUYV 1080p60 (non-compresse)" },
            { UVC_FRAME_FORMAT_YUYV,  1280,  720, 30, "YUYV 720p30 (non-compresse)"  },
            // Repli USB2 : MJPEG compresse mais 60 fps
            { UVC_FRAME_FORMAT_MJPEG, 1280,  720, 60, "MJPEG 720p60"  },
            { UVC_FRAME_FORMAT_MJPEG, 1920, 1080, 30, "MJPEG 1080p30" },
            { UVC_FRAME_FORMAT_YUYV,  1280,  720, 15, "YUYV 720p15 (repli USB2)" },
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

// Decalage du point de consigne (px du crop). Y positif = plus bas, negatif = plus haut.
extern "C" JNIEXPORT void JNICALL
Java_com_example_newtyvision_MainActivity_setAimOffset(
        JNIEnv* /*env*/, jobject /*this*/, jint x, jint y) {
    aimOffsetX.store(x);
    aimOffsetY.store(y);
}

// Lissage EMA de la consigne : 0 = tres lisse (memoire longue), 100 = brut (aucun lissage).
extern "C" JNIEXPORT void JNICALL
Java_com_example_newtyvision_MainActivity_setLissage(
        JNIEnv* /*env*/, jobject /*this*/, jint pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lissagePct.store(pct);
}

// Hauteur de la bande "tete" (% du haut de la silhouette moyenne pour viser). 5..50 conseille.
extern "C" JNIEXPORT void JNICALL
Java_com_example_newtyvision_MainActivity_setHeadBand(
        JNIEnv* /*env*/, jobject /*this*/, jint pct) {
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    headBandPct.store(pct);
}

// Stabilite du boxing (inutilise dans la version simple, conserve pour compat).
extern "C" JNIEXPORT void JNICALL
Java_com_example_newtyvision_MainActivity_setStabilite(
        JNIEnv* /*env*/, jobject /*this*/, jint pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    stabilitePct.store(pct);
}

// Seuil minimum de pixels d'un blob pour etre considere comme cible (anti-parasites).
extern "C" JNIEXPORT void JNICALL
Java_com_example_newtyvision_MainActivity_setMinPixels(
        JNIEnv* /*env*/, jobject /*this*/, jint px) {
    if (px < 0) px = 0;
    minPixels.store(px);
}

// Avance de prediction en ms (compense la latence : on vise ou la cible sera).
extern "C" JNIEXPORT void JNICALL
Java_com_example_newtyvision_MainActivity_setPrediction(
        JNIEnv* /*env*/, jobject /*this*/, jint ms) {
    if (ms < 0) ms = 0;
    if (ms > 300) ms = 300;
    predictionMs.store(ms);
}

// Taille de la zone detectee (% de largeur et de hauteur de l'image, centree).
extern "C" JNIEXPORT void JNICALL
Java_com_example_newtyvision_MainActivity_setZoneSize(
        JNIEnv* /*env*/, jobject /*this*/, jint pctX, jint pctY) {
    if (pctX < 3) pctX = 3; if (pctX > 100) pctX = 100;
    if (pctY < 3) pctY = 3; if (pctY > 100) pctY = 100;
    cropPctX.store(pctX);
    cropPctY.store(pctY);
}

// Pipette : renvoie le HSV moyen au centre du viseur [H, S, V].
extern "C" JNIEXPORT jintArray JNICALL
Java_com_example_newtyvision_MainActivity_getCenterHsv(JNIEnv* env, jobject /*this*/) {
    jint v[3] = { centreH.load(), centreS.load(), centreV.load() };
    jintArray a = env->NewIntArray(3);
    env->SetIntArrayRegion(a, 0, 3, v);
    return a;
}

// Renvoie [trouvee(0/1), X, Y, seq, boxW, boxH] : ecart px (detection HSV) + numero de
// trame + taille de la box cible. Lu par Kotlin pour l'affichage et l'envoi au MAKCU.
extern "C" JNIEXPORT jintArray JNICALL
Java_com_example_newtyvision_MainActivity_getTarget(JNIEnv* env, jobject /*this*/) {
    jint vals[6];
    vals[0] = targetFound.load() ? 1 : 0; vals[1] = targetX.load(); vals[2] = targetY.load();
    vals[3] = frameSeq.load(); vals[4] = boxW.load(); vals[5] = boxH.load();
    jintArray arr = env->NewIntArray(6);
    env->SetIntArrayRegion(arr, 0, 6, vals);
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
