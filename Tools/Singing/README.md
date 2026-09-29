# Songs made with Suno, sung by Revia

Revia's karaoke runtime performs recordings that already exist: an instrumental track,
a vocal track, and timed lines. Her speaking voice cannot sing a melody. The way to hear
her sing a song of yours is therefore:

1. **Make the song with Suno** (or any tool whose output you have the rights to) and
   download it, ideally as WAV. Songs you generate yourself carry no third-party
   performance rights problem, which is the whole reason this pipeline starts there.
2. **Separate** the vocal from the instrumental.
3. **Convert** the vocal into Revia's voice with an RVC model trained on her own speech.
4. **Time the lyrics** so the karaoke lines follow the song.
5. **Drop the result** into her songs folder. `/sing bright-lights`, or "Revia, sing Bright
   Lights".

`prepare_song.py` does steps 2 to 5 in one go and writes a descriptor that says where the
recording came from and whose voice sings it; Revia announces both ("Bright Lights (made
with Suno, voice: Revia (RVC))") and `/songs` lists them.

## Install

```powershell
.\Tools\InstallSinging.ps1
```

This makes `ThirdParty\Singing\.venv` with PyTorch (CUDA 12.8), python-audio-separator and
rvc-python. The separator's RoFormer weights download on first use. `prepare_song.py
--check` says what is installed.

## Her voice: training the RVC model

RVC learns a voice from ten to twenty minutes of clean speech. Make that speech with her
own Qwen3-TTS voice. Revia's own worker takes a key that exists only for her session, so
start a worker of your own for the build (close Revia first, or use another port):

```powershell
.\ThirdParty\QwenTTS\.venv\Scripts\python.exe .\Tools\qwen_tts_service.py --port 8093 --token dataset
```

Then, in a second window:

```powershell
.\ThirdParty\Singing\.venv\Scripts\python.exe .\Tools\Singing\build_voice_dataset.py `
    --script .\lines.txt --out .\dataset\revia --worker-url http://127.0.0.1:8093 --token dataset `
    --reference-audio .\build\debug\RuntimeData\Voices\revia-bright\reference.wav `
    --reference-text "<that clip's transcript>"
```

`lines.txt` is any text of sentences for her to say; varied sentences make a better voice.
The reference clip and its transcript are the ones the voice preset was made from
(`RuntimeData\Voices\<preset>\`).
Then train with [Applio](https://github.com/IAHispano/Applio) or the RVC WebUI: point it
at `dataset\revia`, train (a few hundred epochs is usual), and export the `.pth` and
`.index`. Keep both somewhere outside the repository.

## Prepare a song

```powershell
.\ThirdParty\Singing\.venv\Scripts\python.exe .\Tools\Singing\prepare_song.py `
    --input "Bright Lights.wav" --title "Bright Lights" `
    --songs-root .\build\debug\RuntimeData\Songs `
    --lyrics "Bright Lights.txt" `
    --voice-model C:\Voices\revia.pth --voice-index C:\Voices\revia.index
```

- `--lyrics` takes an `.lrc` (copied as it is) or a plain `.txt` (timed against the
  separated vocal through the local whisper server, `--whisper-url`). Without it the song
  plays with no karaoke lines.
- `--keep-original-voice` skips the conversion and records `"voice": "original"`, so a
  song is never announced as hers when it is not.
- `--pitch` shifts the conversion in semitones when Suno's singer sits far from her range.
- `--overwrite` replaces a song folder you prepared before.

What lands in `RuntimeData/Songs/<id>/`:

```text
instrumental.wav   the backing track
vocal.wav          the vocal in her voice (or the original, if kept)
lyrics.lrc         timed lines, read when song.json marks no sections
song.json          title, credit, madeWith, voice, license, gains
```

## Test the tool without any model

```powershell
py -3 .\Tests\prepareSong.test.py
```

The tests cover the parts that need no audio stack: song ids, lyric timing against
transcribed segments, the descriptor, the plan, and the dataset builder against a fake
voice worker. Separation and conversion are exercised only by preparing a real song; the
first one is worth listening to with `/sing check <id>` before `/sing <id>`.
