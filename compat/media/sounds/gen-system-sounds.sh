#!/usr/bin/env bash
# Gesher's default ringtone, notification and alarm sounds (docs/media.md):
# synthesized with ffmpeg (no recordings, nothing to license), Ogg Vorbis, in
# <out>/media/audio/{ringtones,notifications,alarms}/ as $ANDROID_ROOT has them
# on a device. The system server's settings point RINGTONE,
# NOTIFICATION_SOUND and ALARM_ALERT at them (AudioService.putDefaultSounds).
#
#   compat/media/sounds/gen-system-sounds.sh <out>
set -euo pipefail
out=$1/media/audio
mkdir -p "$out/ringtones" "$out/notifications" "$out/alarms"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

# Notification: one bell-like E6 (1319 Hz), dying away over 0.6 s.
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*1319*t)*exp(-4*t):s=48000:c=mono:d=0.6" \
  -c:a libvorbis -q:a 5 -metadata title="Gesher Notification" \
  "$out/notifications/Gesher_Notification.ogg"
# Ringtone: E5 and A5 (659, 880 Hz), a quarter second each, struck and fading,
# four times over (2 s).
ff -f lavfi -i "aevalsrc=0.4*sin(2*PI*if(lt(mod(t\,0.5)\,0.25)\,659\,880)*t)*exp(-6*mod(t\,0.25)):s=48000:c=mono:d=2" \
  -c:a libvorbis -q:a 5 -metadata title="Gesher Ring" "$out/ringtones/Gesher_Ring.ogg"
# Alarm: B5 (988 Hz) beeps, 0.3 s on and 0.2 s off, for 2 s.
ff -f lavfi -i "aevalsrc=0.45*sin(2*PI*988*t)*lt(mod(t\,0.5)\,0.3):s=48000:c=mono:d=2" \
  -c:a libvorbis -q:a 5 -metadata title="Gesher Alarm" "$out/alarms/Gesher_Alarm.ogg"
ls -lR "$out"
