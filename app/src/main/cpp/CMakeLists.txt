# Version minimum de CMake requise par Android Studio
cmake_minimum_required(VERSION 3.22.1)

# Nom de ton projet (doit correspondre au System.loadLibrary("newtyvision") en Kotlin)
project("newtyvision")

include(FetchContent)

# ==========================================
# 1. LIBUSB (fork CMake, compatible Android : ouverture par file descriptor)
# ==========================================
FetchContent_Declare(
        libusb
        GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
        GIT_TAG        main
)
# On ne veut ni les tests ni les exemples de libusb
set(LIBUSB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBUSB_BUILD_TESTING  OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libusb)

# ==========================================
# 2. LIBUVC (on compile ses sources directement dans notre .so)
#    On evite son CMake maison qui exige pkg-config/libusb systeme (absent sur Android).
# ==========================================
FetchContent_Declare(
        libuvc
        GIT_REPOSITORY https://github.com/libuvc/libuvc.git
        GIT_TAG        master
)
# On telecharge SEULEMENT les sources (pas de MakeAvailable : son CMakeLists exige
# pkg-config/libjpeg, absents sous Windows). On compile ses .c nous-memes plus bas.
FetchContent_GetProperties(libuvc)
if(NOT libuvc_POPULATED)
    FetchContent_Populate(libuvc)
endif()

# Generation du header de config attendu par libuvc.
# On decode le MJPEG avec OpenCV, donc libuvc n'a PAS besoin de libjpeg (LIBUVC_HAS_JPEG non defini).
set(libuvc_VERSION_MAJOR 0)
set(libuvc_VERSION_MINOR 0)
set(libuvc_VERSION_PATCH 7)
set(libuvc_VERSION "0.0.7")
configure_file(
        "${libuvc_SOURCE_DIR}/include/libuvc/libuvc_config.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/libuvc/libuvc_config.h"
        @ONLY)

# Liste des sources libuvc (frame-mjpeg.c volontairement exclu : pas de decode JPEG interne)
set(LIBUVC_SOURCES
        "${libuvc_SOURCE_DIR}/src/ctrl.c"
        "${libuvc_SOURCE_DIR}/src/ctrl-gen.c"
        "${libuvc_SOURCE_DIR}/src/device.c"
        "${libuvc_SOURCE_DIR}/src/diag.c"
        "${libuvc_SOURCE_DIR}/src/frame.c"
        "${libuvc_SOURCE_DIR}/src/init.c"
        "${libuvc_SOURCE_DIR}/src/misc.c"
        "${libuvc_SOURCE_DIR}/src/stream.c")

# ==========================================
# 3. OPENCV
# ==========================================
# On pointe vers le dossier exact que tu as extrait, avec les slashs corriges
set(OpenCV_DIR "C:/Users/newty/Desktop/apkmobile/OpenCV-android-sdk/sdk/native/jni")
find_package(OpenCV REQUIRED)

# ==========================================
# 4. CREATION DE TA LIBRAIRIE NATIVE
# ==========================================
add_library(
        newtyvision
        SHARED
        native-lib.cpp
        ${LIBUVC_SOURCES})

# Chemins d'inclusion : entetes libuvc + le header de config genere ci-dessus
target_include_directories(
        newtyvision PRIVATE
        "${libuvc_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")

# ==========================================
# 5. IMPORTATION DES OUTILS ANDROID
# ==========================================
find_library(log-lib log)
find_library(android-lib android)  # ANativeWindow (rendu sur la Surface)

# ==========================================
# 6. LIAISON (LE PONT FINAL)
# ==========================================
# On soude ton code avec OpenCV, libusb, les logs Android
target_link_libraries(
        newtyvision
        ${OpenCV_LIBS}
        usb-1.0
        ${android-lib}
        ${log-lib})
