#!/bin/sh
# Teste automático: reinicia o jogo, abre Servers, escolhe o primeiro servidor da lista e toca em Join.
#
#   android/test-server.sh [segundos] [apk]
#
# Coordenadas do Galaxy S23 em paisagem (2340x1080). Com mais de um aparelho,
# defina ANDROID_SERIAL.
set -eu
WAIT=${1:-20}
APK=${2:-}
OUT=${OUT:-$PWD/test-server-out}
TAP_INSTANCE=${TAP_INSTANCE:-"1439 359"}
TAP_LAUNCH=${TAP_LAUNCH:-"1602 327"}
TAP_SERVERS=${TAP_SERVERS:-"150 682"}
TAP_SERVER1=${TAP_SERVER1:-"819 567"}   # primeiro servidor da lista Discover
TAP_JOIN=${TAP_JOIN:-"2165 1030"}
mkdir -p "$OUT"

# Only tap while Hyprism is in front, so a crash never sends taps to other apps.
tap() {
    for i in 1 2 3 4 5 6 7 8; do
        adb shell dumpsys window | grep mCurrentFocus | grep -q org.hyprism && break
        [ "$i" = 8 ] && { echo "Hyprism não está em primeiro plano, abortando" >&2; exit 1; }
        sleep 1
    done
    adb shell input tap "$@"
}

adb shell am force-stop org.hyprism.game
[ -n "$APK" ] && adb install -r "$APK" >/dev/null
adb shell am force-stop org.hyprism.launcher
adb shell input keyevent KEYCODE_WAKEUP
adb shell monkey -p org.hyprism.launcher -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sleep 14
tap $TAP_INSTANCE
sleep 5
adb logcat -c
tap $TAP_LAUNCH
for i in $(seq 1 60); do
    adb logcat -d -s Hytale:V | grep -q "launched by Hyprism" && break
    sleep 1
done
sleep 25                                  # espera o menu principal
tap $TAP_SERVERS
sleep 6                                   # espera a lista de servidores
adb exec-out screencap -p > "$OUT/1-servidores.png"
tap $TAP_SERVER1
sleep 2
tap $TAP_JOIN
sleep "$WAIT"
adb exec-out screencap -p > "$OUT/2-servidor.png"
adb shell pidof org.hyprism.game >/dev/null && echo "jogo rodando" || echo "jogo fechou"
adb logcat -d -s HytaleLoader:V Hytale:V MobileGlues:V | sed -E 's/eyJ[A-Za-z0-9._-]+/<token>/g' > "$OUT/logcat.log"
echo "saída em $OUT"
