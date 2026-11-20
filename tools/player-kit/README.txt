Grabbed by the Ghoulies QilinRecomp for Windows (64-bit)
======================================================

Grabbed by the Ghoulies (Rare / Microsoft Game Studios, Xbox, 2003),
recompiled to run natively: the game's own program translated into C++ and
compiled for your PC, drawn with Vulkan.

No game data is included. You need your own copy of the game: the USA
release (MS-083), its disc image extracted to a folder (for example with
extract-xiso or xdvdfs). That gives a folder holding default.xbe and the
folders bundles, demand, xwavebank and the rest.

This is a work in progress: the game plays, and some things are still
missing or wrong. Changes: CHANGELOG.md on the project's page.


Playing
-------
1. Unpack this folder anywhere (it needs about 2 GB once built).
2. Double-click "Play Ghoulies.bat".
   - The first time, it asks for your extracted disc folder (the one that
     holds default.xbe), then builds the game from it: 5 to 20 minutes,
     depending on the PC. Your game files never leave your PC.
   - After that, it starts the game straight away.
   - To use a different disc folder: Play Ghoulies.bat -ChooseDisc
   - Ghoulies.exe in this folder can be started directly too:
       Ghoulies.exe --game-dir "<disc folder>" "<disc folder>\default.xbe"
3. What the game prints goes into run.log in this folder: send that file
   when you report a problem.

If Windows says "Windows protected your PC": More info, then Run anyway.
(The download isn't signed.) Or before unpacking, right-click the zip,
Properties, tick Unblock, OK.

Needs: Windows 10 (version 1803 or later) or 11, 64-bit, and a graphics
driver with Vulkan 1.3 (any recent NVIDIA, AMD or Intel driver).


Controls
--------
Any controller Windows or SDL knows (Xbox, PlayStation, Switch, generic),
as the Xbox controller, or the keyboard as controller 1:

  Xbox controller      Keyboard
  left stick           W A S D
  right stick          numeric keypad 8 4 5 6
  D-pad                arrows
  A B X Y              Space or K, L, J, I
  White, Black         Q, E   (left and right shoulder buttons)
  triggers             Left Shift, Right Shift or O
  Start, Back          Enter, Backspace or Tab
  stick clicks         F, H

F1 or Esc, or Back and Start together on a controller: the settings menu
(the game pauses). Display: full screen, the aspect ratio (auto: your
screen's shape; 4:3 the original; 16:10, 16:9, 21:9 and 32:9 show more at the
sides), how the picture fills the window, the HUD on wide shapes (at the screen's edges, in the
middle 4:3, or stretched), the render resolution (auto, or 1x to 4x), smooth or
sharp pixels, SMAA anti-aliasing, AMD FSR 1 upscaling with its sharpness, a CRT television look, vsync, the frame rate (30 as
the console, 60, your display's refresh rate, or unlimited), low latency, anisotropic texture
filtering. Controls: rumble, and the keyboard's keys (set by pressing them).
Game: volume, stereo or 5.1, the game's language.
F11 or Alt+Enter: full screen. The window can be resized: the picture keeps
its shape.

Saves and settings are kept in %LOCALAPPDATA%\qilin\ghoulies (the settings
in ghoulies.ini, which can be edited by hand too).


What's in here
--------------
Play Ghoulies.bat   builds (once) and starts the game
kit\qilin.exe       the recompiler: turns the disc's default.xbe into C++
kit\lib, include    the prebuilt rest of the game: its runtime, the Xbox's
                    kernel, Direct3D and DirectSound replaced natively, the
                    renderer (Vulkan), the window, controls and sound (SDL3),
                    the settings menu (Dear ImGui)
kit\toolchain       LLVM-MinGW, a C++ compiler for Windows (Clang)
kit\ninja.exe       Ninja, which drives the compiler
work\               made when building: the recompiled code (from your disc)
LICENSES\           the licences of everything included

Source, changes and issues: https://github.com/Qilin1066/GBTGhoulies-QilinRecomp
