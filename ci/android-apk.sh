#!/bin/bash
# P2Play for Android: the server p2playd per ABI (NDK, Android 6+), packaged as
# lib/<abi>/libp2playd.so into one APK (android/).
#
# usage: ci/android-apk.sh <boost headers>   (ANDROID_NDK_HOME, ANDROID_HOME, java 17)
# out: release/p2play-android.apk, release/<abi>/p2playd (unstripped)
set -euo pipefail

BOOST="$1"
ROOT="$(pwd)"
ABIS="arm64-v8a armeabi-v7a"
JNI="$ROOT/android/app/src/main/jniLibs"

rm -rf "$JNI" "$ROOT/release"
for abi in $ABIS; do
    cmake -S "$ROOT" -B "$ROOT/build-android-$abi" -G Ninja -DPLATFORM_ANDROID=ON -DANDROID_ABI="$abi" \
        -DCMAKE_BUILD_TYPE=Release -DBOOST_INCLUDE_DIR="$BOOST"
    cmake --build "$ROOT/build-android-$abi" --target p2play -j"$(nproc)"
    mkdir -p "$JNI/$abi" "$ROOT/release/$abi"
    cp "$ROOT/build-android-$abi/src/app/p2playd" "$ROOT/release/$abi/p2playd"
    "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip \
        -o "$JNI/$abi/libp2playd.so" "$ROOT/release/$abi/p2playd"
done

cd "$ROOT/android"
chmod +x gradlew
./gradlew --no-daemon assembleRelease
cp app/build/outputs/apk/release/app-release.apk "$ROOT/release/p2play-android.apk"
ls -lR "$ROOT/release"
