# Revia Bright nonverbal audio

Generated locally on 2026-09-06 with Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign,
using the Revia Bright description in the default voice catalogue plus an
instruction for each sound. PCM16 mono WAV, 24 kHz. Eleven clips cover all six
supported sounds, with two variants except for the gasp. The retained clips
are 0.48–1.68 seconds long; an overlong gasp variant was discarded.

These are persistent voice assets. Runtime bootstrap installs missing clips
for existing and new Revia Bright voices without replacing the catalogue,
reference audio, or any previously installed clip. Speech playback never
synthesizes the marker word and never deletes a bank clip after use.
