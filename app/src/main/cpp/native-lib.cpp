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
static Scalar HSV_LOW (133, 43, 106);
static Scalar HSV_HIGH(160, 156, 206);

// Rayon de regroupement des clusters (0 = pixels colles seulement, plus grand = fusion large)
static std::atomic<int> clusterRadius{10};

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

    // On lit les seuils courants (copie rapide sous verrou)
    Scalar bas, haut;
    {
        std::lock_guard<std::mutex> lock(hsvMutex);
        bas = HSV_LOW;
        haut = HSV_HIGH;
    }

    // --- DETECTION COULEUR (effet "color splash") sur toute l'image ---
    // 1. Masque : 255 la ou le pixel est dans ta plage HSV, 0 ailleurs
    cvtColor(frameBrute, hsv, COLOR_BGR2HSV);
    inRange(hsv, bas, haut, mask);

    // 2. Fond en niveaux de gris (format RGBA pour la Surface)
    cvtColor(frameBrute, gray, COLOR_BGR2GRAY);
    cvtColor(gray, resultat, COLOR_GRAY2RGBA);

    // 3. On reintroduit la couleur d'origine la ou le masque est actif
    cvtColor(frameBrute, couleurRGBA, COLOR_BGR2RGBA);
    couleurRGBA.copyTo(resultat, mask);

    // 4. Regroupement en clusters + une boite verte par cluster
    // On DILATE le masque : deux taches proches se rejoignent (= meme cluster),
    // deux taches eloignees restent separees. Le rayon vient du slider (0-100).
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

    Rect limites(0, 0, resultat.cols, resultat.rows);
    for (auto& c : contours) {
        if (contourArea(c) < 80.0) continue; // anti-bruit
        Rect boite = boundingRect(c);
        // La dilatation a gonfle la boite de r sur chaque bord : on la resserre.
        if (r > 0) {
            boite.x += r; boite.y += r;
            boite.width -= 2 * r; boite.height -= 2 * r;
        }
        boite &= limites;
        if (boite.width > 2 && boite.height > 2) {
            rectangle(resultat, boite, Scalar(0, 255, 0, 255), 3); // vert
        }
    }

    // 5. Affichage
    afficherSurSurface(resultat);
}

// ==========================================
// NEGOCIATION DE FORMAT (avec repli automatique)
// On essaie MJPEG (haute cadence) puis YUYV, a plusieurs resolutions.
// ==========================================
static bool negocierFormat(uvc_stream_ctrl_t* ctrl) {
    struct Essai { uvc_frame_format fmt; int w; int h; int fps; const char* nom; };
    const Essai essais[] = {
            { UVC_FRAME_FORMAT_MJPEG, 1920, 1080, 60, "MJPEG 1080p60" },
            { UVC_FRAME_FORMAT_MJPEG, 1920, 1080, 30, "MJPEG 1080p30" },
            { UVC_FRAME_FORMAT_MJPEG, 1280,  720, 60, "MJPEG 720p60"  },
            { UVC_FRAME_FORMAT_MJPEG, 1280,  720, 30, "MJPEG 720p30"  },
            { UVC_FRAME_FORMAT_YUYV,  1280,  720, 30, "YUYV 720p30"   },
            { UVC_FRAME_FORMAT_YUYV,   640,  480, 30, "YUYV 480p30"   },
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

// ==========================================
// 5. TEST DE LIAISON
// ==========================================
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_newtyvision_MainActivity_stringFromJNI(JNIEnv* env, jobject /*this*/) {
    std::string hello = "Moteur C++ NewtyVision Connecte !";
    return env->NewStringUTF(hello.c_str());
}
