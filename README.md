# Counter-Strike 2 Sound Player

Listen to CS2 events and key presses to play audio files in game automatically

<img src="https://github.com/NxRoot/cs2-sound-player/blob/main/preview/banner.png" width="90%" alt="Alt text" title="icon">

# Features
- 💫 Automatic - Waits for game to open.
- 💯 Undetectable - External tool, no injection.
- 🧬 Self-Healing - No rebuild after game update.
- 🎵 Multiple Audios - Play random sound from folder.

# How to Install

| Exe    | Description | Releases |
| -------- | ------- | ------- |
| <a href="https://github.com/NxRoot/cs2-sound-player/releases"><img style="min-width: 40px;min-height: 40px; width: 40px;" src="https://github.com/NxRoot/cs2-sound-player/blob/main/icon.ico"/></a> | CS2 Audio Player    | [Download](https://github.com/NxRoot/cs2-sound-player/releases)    |

# Default Events
Events are defined by folders inside `sounds`, each folder should contain the audio files to be played.

| Event    | Description | Overlap |
| -------- | ------- | ------- |
|<a href="#">onDeath</a>| Triggered when player dies| False |
|<a href="#">onKill</a>| Triggered when player kills an enemy| False |
|<a href="#">onRoundFreeze</a>| Triggered when player freezes before round start| True |
|<a href="#">onRoundStart</a>| Triggered when round starts and player unfreezes| True |
|<a href="#">onRoundLose</a>| Triggered when player team loses the round| True |
|<a href="#">onRoundWin</a>| Triggered when player team wins the round| True |
|<a href="#">C</a>| Triggered when player presses C on keyboard| True |

# Custom Events
You can create key press events by creating a folder inside `sounds` with the letter you want to trigger.

# Kill Streak Events
You can create kill streak events by creating numeric folders inside `onKill` folder:
```ini
sounds
  └── onKill                          # Event Folder
      ├── 1                           # First Kill (Folder)
          └── first_kill.mp3          # First Kill (Sound)
      ├── 2                           # Second Kill (Folder)
          └── second_kill.mp3         # Second Kill (Sound)
      ├── 3                           # Third Kill (Folder)
          ├── third_kill_A.mp3        # Third Kill (Sound A)
          └── third_kill_B.mp3        # Third Kill (Sound B)
      ├── other_A.mp3
      ├── other_B.mp3
      └── other_C.mp3
```
If you kill &nbsp;  `1 enemy` &nbsp;  play random sound from folder &nbsp;`1`.

If you kill &nbsp;  `2 enemies` &nbsp;  play random sound from folder &nbsp;`2`.

If there's no more numeric folders, play random sound from `onKill` folder.

# Output to Microphone

* To **emulate a microphone device** you need to install a virtual audio driver like [VB-CABLE](https://vb-audio.com/Cable/)
* Then you can select the microphone (Cable Output) inside the game.


# In Game Settings
* Set voice mode to `Open Microphone`, not push to talk.
* Set threshold above noise, around `-90`. (Optional)
<img src="https://github.com/NxRoot/cs2-sound-player/blob/main/preview/settings.png" width="80%" alt="Alt text" title="icon">


## &nbsp;
⭐ If you find this useful!
