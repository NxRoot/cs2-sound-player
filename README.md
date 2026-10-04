# Counter-Strike 2 Sound Player

Listen to CS2 events and key presses to play audio files in game automatically

<img src="https://i.ibb.co/93VVY3Xm/CS2-Sound-Player-in-Dust-II.png" width="70%" alt="Alt text" title="icon">

# Features
- 💫 Automatic - Waits for game to open.
- 💯 Undetectable - External tool, no injection.
- 🧬 Self-Healing - No rebuild after game update.

# How to Install

| Exe    | Description | Releases |
| -------- | ------- | ------- |
| <a href="https://github.com/NxRoot/cs2-sound-player/releases"><img style="min-width: 40px;min-height: 40px; width: 40px;" src="https://i.ibb.co/8ngSJrBs/icon.jpg"/></a> | CS2 Audio Player    | [Download](https://github.com/NxRoot/cs2-sound-player/releases)    |

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

# Output to Microphone

* To **emulate a microphone device** you need to install a virtual audio driver like [VB-CABLE](https://vb-audio.com/Cable/).
* Then you can select the microphone (Cable Input) inside the game.


## &nbsp;
⭐ If you find this useful!
