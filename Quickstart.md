# System Requirements

**Meeting recommended specifications won't guarantee perfect performance.**

Recommended |   |
----------- | -
OS          | Windows 11 x64 or later
CPU         | 6 or more cores<br>You can check with [CPU-Z](https://www.cpuid.com/softwares/cpu-z.html)
GPU         | NVIDIA GTX 980 Ti or later<br>You can check with [GPU-Z](https://www.techpowerup.com/download/techpowerup-gpu-z/)
RAM         | 6 GB or more

[Microsoft Visual C++ Redistributable for Visual Studio 2015-2022](https://aka.ms/vs/17/release/vc_redist.x64.exe)

Minimum |   |
------- | -
OS      | Windows 10 x64 or later<br>Linux with Proton. Vulkan is recommended due to VKD3D potentially having graphical issues.
CPU     | 64-bit x86 processor with AVX or AVX2 support<br>You can check with [CPU-Z](https://www.cpuid.com/softwares/cpu-z.html)
GPU     | Direct3D **12**-compatible or **Vulkan**-compatible GPU<br>You can check with [GPU-Z](https://www.techpowerup.com/download/techpowerup-gpu-z/)<br>OpenGL and Direct3D 11 or lower are not, and never will be supported.<br>AMD GPUs are not recommended due to having driver bugs that can cause crashes with Xenia.<br>Integrated GPUs generally provide frame rates too low for comfortable playing.
RAM     | 4 GB

[2015-2022 x64 Visual C++ Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe)

Xenia doesn't require any Xbox 360 system files.

Xenia checks for the minimum supported CPU and GPU on startup and errors out
if one is not detected. Make sure that you have the latest drivers installed. **You cannot just remove the checks in the code and assume things will work.**

***There are no magical workarounds to make Xenia work on your potato computer.***

On Android, Xenia can only render GPU traces so it can't play games yet. Anything claiming to be a Xenia apk, etc is fake and probably a virus.

Windows Update tends to lag behind, so download GPU drivers from your manufacturer:
  * **[AMD](https://www.amd.com/en/support)**
  * **[NVIDIA](https://www.nvidia.com/Download/index.aspx)**
  * **[Intel](https://downloadcenter.intel.com/product/80939/Graphics-Drivers)**

## Where do I download Xenia?

  * **[Xenia Canary](https://github.com/xenia-canary/xenia-canary/releases/download/experimental/xenia_canary.zip)**
* <details><summary>Xenia Master (click to expand)</summary>
  Xenia Master is infrequently updated, but if you're experiencing problems with Canary you can try it:

  *[Xenia Master](https://github.com/xenia-project/release-builds-windows/releases/latest/download/xenia_master.zip)*
</details>

Xenia is sometimes detected as a virus. If it was downloaded from an official source it is a false positive and can be ignored.

Xenia Canary is a fork of Xenia that is more frequently updated and has changes not present in master.

For more information: *[What is the difference between Xenia Canary and Xenia master?](https://github.com/xenia-canary/xenia-canary/wiki/FAQ#what-is-the-difference-between-xenia-canary-and-xenia-master)*

# How to rip games

**YOU CANNOT PLAY GAMES DIRECTLY FROM THE DISC DRIVE, OR USE AN XBOX ONE/SERIES TO RIP GAMES!**

## Stock/unmodded console method

### Requirements

* Xbox 360 console
  * If you want to rip discs it must be able to read them.
  * Dashboards older than 17511 might be different.
* USB storage device that's at least as big as the games themselves, preferrably larger. You will encounter problems with smaller devices.
* [Velocity](https://github.com/Gualdimar/Velocity/releases/download/xex%2Biso-branch/Velocity-XEXISO.rar)

### Preparation

Before beginning, you need to check if the USB is formatted:

1. Go to *[Settings > System](https://i.imgur.com/xcCn6fM.png) > [Storage](https://i.imgur.com/No4y9xi.png)*
<details><summary>Images (click to expand)</summary>

Settings > System:
![](https://i.imgur.com/xcCn6fM.png)
Storage:
![](https://i.imgur.com/No4y9xi.png)</details>
2. If you see an [Unformatted](https://i.imgur.com/Jex2sln.png) storage device, select it.
<details><summary>Image (click to expand)</summary>

Unformatted
![](https://i.imgur.com/Jex2sln.png)</details>

* If it's already formatted, skip this section.
3. [Press A to format](https://i.imgur.com/tIW9spr.png)
4. [Select Yes](https://i.imgur.com/rKvf04S.png)
<details><summary>Images (click to expand)</summary>

Press A to format:
![](https://i.imgur.com/tIW9spr.png)
Select Yes:
![](https://i.imgur.com/rKvf04S.png)</details>
* **Note: This will erase everything from the drive. Make sure to move the files off of it first!**
5. Once done, return to home by pressing B twice.

### Section 1. Ripping discs

*If your game isn't a disc (XBLA, digital, etc.) skip to Section 2.*

You'll want to change these options to stop game(s) from automatically starting:
  * *[Settings > System](https://i.imgur.com/xcCn6fM.png) > [Console Settings](https://i.imgur.com/FStw2Y7.png) > [Auto-Play](https://i.imgur.com/r4lLczk.png) > [Disable](https://i.imgur.com/V5oEdQl.png)*
<details><summary>Images (click to expand)</summary>

Settings > System:
![](https://i.imgur.com/xcCn6fM.png)
Console Settings:
![](https://i.imgur.com/FStw2Y7.png)
Auto-Play:
![](https://i.imgur.com/r4lLczk.png)
Disable:
![](https://i.imgur.com/V5oEdQl.png)</details>
* *[Settings > System](https://i.imgur.com/xcCn6fM.png) > [Console Settings](https://i.imgur.com/FStw2Y7.png) > [Startup and Shutdown](https://i.imgur.com/DgblBFS.png) > [Startup](https://i.imgur.com/GJpqOrH.png) > [Xbox Dashboard](https://i.imgur.com/H4ffGAV.png)*
<details><summary>Images (click to expand)</summary>

Settings > System:
![](https://i.imgur.com/xcCn6fM.png)
Console Settings:
![](https://i.imgur.com/FStw2Y7.png)
Startup and Shutdown:
![](https://i.imgur.com/DgblBFS.png)
Startup:
![](https://i.imgur.com/GJpqOrH.png)
Xbox Dashboard:
![](https://i.imgur.com/H4ffGAV.png)</details>
1. Go to home on the dashboard
2. Insert the disc into the drive, and close the tray.
3. Once the game shows up press X (Game Details) with the game selected.
4. Press the *Install* button.
    * If you see *Delete* instead, skip to Section 2.
      * Note: Games with the [no-disc-install label](https://github.com/xenia-canary/game-compatibility/issues?q=is%3Aopen+is%3Aissue+label%3Ano-disc-install) can't be installed this way, and require a modded console or using the Redump method.
5. Select the storage device you want to install the game on.
6. Once it's 100% Completed press A to continue, and press B to go back to home.

### Section 2. Transferring HDD games

***If you ripped the game(s) directly to the USB drive, or they're already on the USB drive, you can skip this section.***
1. Go to *[Settings > System](https://i.imgur.com/xcCn6fM.png) > [Storage](https://i.imgur.com/No4y9xi.png) > [Hard Drive](https://i.imgur.com/8EB0EFr.png) > [Press Y (Device Options)](https://i.imgur.com/rRaoeAR.png) > [Transfer Content](https://i.imgur.com/wdvYqDR.png) > [USB Storage Device](https://i.imgur.com/6FVly57.png)*
4. Choose what you want to transfer. Keep in mind full games and trials/demos are in separate categories.
<details><summary>Images (click to expand)</summary>

Settings > System:
![](https://i.imgur.com/xcCn6fM.png)
Storage:
![](https://i.imgur.com/No4y9xi.png)
Hard Drive:
![](https://i.imgur.com/8EB0EFr.png)
Press Y (Device Options):
![](https://i.imgur.com/rRaoeAR.png)
Transfer Content:
![](https://i.imgur.com/wdvYqDR.png)
USB Storage Device:
![](https://i.imgur.com/6FVly57.png)</details>
5. Press right, then *[Start](https://i.imgur.com/Gpb5Zya.png)*.
<details><summary>Image (click to expand)</summary>

Start:
![](https://i.imgur.com/Gpb5Zya.png)</details>
Once it's done you can take the USB out of the console.

### Section 3. Importing games for use in Xenia

1. Insert the USB drive into your PC. Go to `Content`.
    * If the USB drive appears empty enable *[Show hidden files and folders](https://support.microsoft.com/en-us/help/14201/windows-show-hidden-files)* in Windows Explorer.
2. Go into `00000#`. There will be folders with names consisting of lots of letters and numbers. You should find the game's folder within one of them.
3. The game will have a name with a bunch of letters and numbers just like the folders, and no file extension.
    * XBLA games will be one file, GOD (disc) games will contain a file, along with a *.data folder with the same name.
    * *ContentCache.pkg is irrelevant. Ignore it.*
 4. To confirm that it is indeed a game try opening the file in Xenia.
 5. Once you've made sure the file works in Xenia you can rename, or move the folder the game is in for convenience.
    * **FOLDER ABOVE, NOT FILE or *.DATA FOLDER!**
      * (Optional) If the game crashes you can try extracting the game using Velocity.
      * If the game is extracted you will need to drag `default.xex` onto Xenia.
      * Some games have multiple .xex files, so if you can't find `default.xex` or it just doesn't work, try another one.

It is not recommended to convert games to ISO due to wasting space.

### Section 4. Activating games

***By default Xenia runs ALL XBLA/digital games in demo/trial mode.***

To run games in full/activated mode you need to change [this option](Options#user-content-Run_games_as_fullactivated).

## Redump method (rare/specific drives)

See http://wiki.redump.org/index.php?title=Microsoft_Xbox_and_Xbox_360_Dumping_Guide

<!--## Modded console method

While you can use a modded console to dump games, it is unnecessary in 99% of cases.

## Requirements

* Modded Xbox 360 console
  * If you want to rip discs it must be able to read them.

-->

# How to install DLCs

1. Identify what the Game Title ID is.
This can be identified by running the game in Xenia.

<details><summary>Image (click to expand)</summary>

![](https://i.imgur.com/fc0rmSc.png)</details>

2. Locate your DLC Content folder from your removable storage.

<details><summary>Image (click to expand)</summary>

![](https://i.imgur.com/t8IMZiG.png)</details>

3. Download Velocity from [here](https://github.com/Gualdimar/Velocity/releases).

4. Open the Packages with Velocity.

<details><summary>Image (click to expand)</summary>

![](https://i.imgur.com/7q7q0oB.png)</details>

5. Extract the content packages
<details><summary>Image (click to expand)</summary>

![](https://i.imgur.com/WyA4yhm.png)</details>
to `Documents\Xenia\TitleID\00000002` and their corresponding folder names.
<details><summary>Image (click to expand)</summary>

![](https://i.imgur.com/e4zk397.png)</details>
