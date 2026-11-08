#!/bin/bash
# Assemble the Gradle project at build/android/app from SDL3's own Android template plus the libraries the
# Makefile already produced.
#
# Gradle does not build any native code here. Everything native is prebuilt - libmain.so from
# `make PLATFORM=android`, lasgame.so from the generator, and the four cross-built dependencies - so the APK step
# is packaging only. That keeps one build system for the C and avoids teaching Gradle about the generator.
set -euo pipefail
WORK=${WORK:-/root/lasrecomp}
NDK=${NDK:-/opt/android-ndk}
SDLT=$WORK/build/android/deps-build/sdl/android-project
DEPS=$WORK/build/android/deps
APP=$WORK/build/android/app
PKG=org.recomp.za
PKGDIR=org/recomp/za
JNI=$APP/app/src/main/jniLibs/arm64-v8a
# GAME_BUILD: the build folder whose libmain.so and lasgame.so go in (build_gamelib_android.sh BREL): one per game
GAME_BUILD=${GAME_BUILD:-$WORK/build/android}

[ -f "$GAME_BUILD/libmain.so" ] || { echo "no libmain.so in $GAME_BUILD: build_gamelib_android.sh first"; exit 1; }

echo "=== project skeleton from SDL3's template ==="
rm -rf "$APP"
mkdir -p "$APP/app/src/main/java/$PKGDIR" "$JNI"
# the Java glue, the Gradle wrapper and the resources come from SDL unchanged: SDLActivity is what loads our
# library and drives the Android lifecycle
cp -R "$SDLT/app/src/main/java/org" "$APP/app/src/main/java/"
cp -R "$SDLT/app/src/main/res" "$APP/app/src/main/"
cp -R "$SDLT/gradle" "$APP/"
cp "$SDLT/gradlew" "$SDLT/gradlew.bat" "$APP/"
chmod +x "$APP/gradlew"

echo "=== our Activity ==="
cat > "$APP/app/src/main/java/$PKGDIR/ZARecompActivity.java" <<JAVA
package $PKG;

import org.libsdl.app.SDLActivity;

/** SDLActivity does the whole Android side; all this adds is which libraries to load, and in what order. */
public class ZARecompActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        // dependencies before dependents: the dynamic linker has no search path inside an APK
        // no "c++_shared": libmain.so carries libc++ statically. Loading the NDK's copy globally crashed
        // HWUI's Vulkan startup inside Qualcomm's gralloc, before any of our own code ran.
        return new String[] {
            "SDL3",
            "avutil", "swscale", "avcodec", "avformat",
            "shaderc_shared",
            "main",            // libmain.so: the runtime, which dlopens lasgame.so itself
        };
    }
}
JAVA

# The launcher's icon starts this one, which starts the game on the main display: a dual-screen handheld (AYN Thor:
# 1920x1080 top, 1080x1240 bottom) would otherwise open the game on whichever screen the launcher was tapped on.
cat > "$APP/app/src/main/java/$PKGDIR/ZALaunchActivity.java" <<JAVA
package $PKG;

import android.app.Activity;
import android.app.ActivityOptions;
import android.content.Intent;
import android.os.Bundle;
import android.view.Display;

/** No window of its own: the game's activity, on the default (top) display, then gone. */
public class ZALaunchActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Intent i = new Intent(this, ZARecompActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i, ActivityOptions.makeBasic().setLaunchDisplayId(Display.DEFAULT_DISPLAY).toBundle());
        finish();
    }
}
JAVA

echo "=== manifest ==="
cat > "$APP/app/src/main/AndroidManifest.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android">

    <!-- Vulkan 1.3 is what the renderer needs; saying so keeps the app off devices that cannot run it -->
    <uses-feature android:name="android.hardware.vulkan.version" android:version="0x403000" android:required="true" />
    <uses-feature android:name="android.hardware.gamepad" android:required="false" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />

    <application
        android:label="ZA Recomp"
        android:icon="@mipmap/ic_launcher"
        android:theme="@style/AppTheme"
        android:allowBackup="false"
        android:hardwareAccelerated="true"
        android:hasFragileUserData="true">

        <activity
            android:name=".ZARecompActivity"
            android:label="ZA Recomp"
            android:exported="true"
            android:alwaysRetainTaskState="true"
            android:launchMode="singleInstance"
            android:screenOrientation="sensorLandscape"
            android:preferMinimalPostProcessing="true"
            android:configChanges="layoutDirection|locale|orientation|uiMode|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation" />

        <activity
            android:name=".ZALaunchActivity"
            android:label="ZA Recomp"
            android:exported="true"
            android:noHistory="true"
            android:excludeFromRecents="true"
            android:theme="@android:style/Theme.NoDisplay">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
XML

echo "=== gradle files ==="
cat > "$APP/settings.gradle" <<'GRADLE'
pluginManagement {
    repositories { google(); mavenCentral(); gradlePluginPortal() }
}
dependencyResolutionManagement {
    repositories { google(); mavenCentral() }
}
rootProject.name = "ZARecomp"
include ':app'
GRADLE

cat > "$APP/build.gradle" <<'GRADLE'
plugins {
    id 'com.android.application' version '8.7.3' apply false
}
GRADLE

cat > "$APP/gradle.properties" <<'GRADLE'
org.gradle.jvmargs=-Xmx4096m
android.useAndroidX=true
android.nonTransitiveRClass=true
GRADLE

cat > "$APP/app/build.gradle" <<'GRADLE'
plugins { id 'com.android.application' }

android {
    namespace "org.recomp.za"
    compileSdk 35

    defaultConfig {
        applicationId "org.recomp.za"
        // 31 because that is what the native code was compiled against (ANDROID_API in the Makefile)
        minSdk 31
        targetSdk 35
        versionCode 1
        versionName "0.1"
        ndk { abiFilters 'arm64-v8a' }
    }

    // No externalNativeBuild: every .so is prebuilt by the Makefile and dropped into jniLibs.
    sourceSets { main { jniLibs.srcDirs = ['src/main/jniLibs'] } }

    packagingOptions {
        jniLibs {
            // Extracted to a real directory on install rather than mmap'd from the APK: the runtime dlopens
            // lasgame.so by path (runtime/gamelib.c), which needs a file to exist. Costs disk, buys simplicity.
            useLegacyPackaging true
        }
    }

    buildTypes {
        debug { debuggable true; jniDebuggable true }
    }

    compileOptions {
        sourceCompatibility JavaVersion.VERSION_17
        targetCompatibility JavaVersion.VERSION_17
    }
}
GRADLE

echo "=== native libraries into jniLibs/arm64-v8a ==="
cp "$GAME_BUILD/libmain.so" "$JNI/"
for l in libSDL3.so libavformat.so libavcodec.so libavutil.so libswscale.so libshaderc_shared.so; do
    cp "$DEPS/lib/$l" "$JNI/"
done
# Thor experiment (Makefile VOLK=1): libadrenotools' hook libraries and the Turnip driver, when staged in deps/adrenotools
if [ -d "$DEPS/adrenotools" ]; then cp "$DEPS/adrenotools/"*.so "$JNI/"; fi
# libc++_shared is deliberately NOT packaged: see the Makefile's android LDFLAGS

echo "=== bundled free fonts into assets/fonts (with their licences) ==="
# the same third_party/fonts files the desktop builds ship beside the executable; runtime/platform/android.c unpacks
# them to internal storage at start (an APK's lib directory only holds .so files). The SIL OFL asks for its text to
# travel with the font, so the licences go too.
A=$APP/app/src/main/assets/fonts
mkdir -p "$A"
cp "$WORK/third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf" "$A/"
cp "$WORK/third_party/fonts/BIZUDPGothic/OFL.txt" "$A/BIZUDPGothic-OFL.txt"
cp "$WORK/third_party/fonts/DejaVu/DejaVuSans.ttf" "$A/"
cp "$WORK/third_party/fonts/DejaVu/LICENSE" "$A/DejaVu-LICENSE.txt"
# (the Snapdragon GSR upscaler's BSD-3 licence asks for its notice with every binary)
mkdir -p "$APP/app/src/main/assets/licenses"
cp "$WORK/third_party/sgsr/LICENSE" "$APP/app/src/main/assets/licenses/SnapdragonGSR-LICENSE.txt"
ls -1 "$A" | sed 's/^/  /'
# the recompiled game itself, if it has been built
if [ -f "$GAME_BUILD/lasgame.so" ]; then
    cp "$GAME_BUILD/lasgame.so" "$JNI/"
else
    echo "  (no lasgame.so yet - the APK will install but have no game code)"
fi

echo
echo "=== jniLibs ==="
ls -lh "$JNI" | tail -n +2 | while read -r _ _ _ _ size _ _ _ name; do printf '  %-26s %s\n' "$name" "$size"; done
echo "  total: $(du -sh "$JNI" | cut -f1)"
echo
echo "project at $APP"
