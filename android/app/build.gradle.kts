plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.opendisplay.app"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.opendisplay.app"
        minSdk = 31
        targetSdk = 35
        versionCode = 2
        versionName = "0.2beta"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}
