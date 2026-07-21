import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
}

// Identifiants materiels sortis du code : lus depuis config/device.properties
// (fichier LOCAL, ignore par git). Repli 0x0000 si absent (voir device.properties.example).
val deviceProps = Properties().apply {
    val f = rootProject.file("config/device.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
fun devId(cle: String): String = deviceProps.getProperty(cle, "0x0000")

android {
    namespace = "com.example.newtyvision"
    compileSdk {
        version = release(36) {
            minorApiLevel = 1
        }
    }

    defaultConfig {
        applicationId = "com.example.newtyvision"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            // On ne compile que pour les appareils reels (ARM 64 bits).
            // Ajoute "armeabi-v7a" si tu vises de vieux appareils 32 bits.
            abiFilters += "arm64-v8a"
        }

        // VID/PID injectes depuis config/device.properties (non versionne).
        buildConfigField("int", "CARD_VID", devId("card.vendorId"))
        buildConfigField("int", "CARD_PID", devId("card.productId"))
        buildConfigField("int", "MAKCU_VID", devId("makcu.vendorId"))
        buildConfigField("int", "MAKCU_PID", devId("makcu.productId"))
    }

    buildTypes {
        release {
            optimization {
                enable = false
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    buildFeatures {
        viewBinding = true
        buildConfig = true
    }
}

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.constraintlayout)
    implementation(libs.androidx.core.ktx)
    implementation(libs.material)
    implementation("com.github.mik3y:usb-serial-for-android:3.8.1") // liaison serie MAKCU (CH34x)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
}