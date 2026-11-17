# Naming

**Qilin** is the author's name. It goes on the *tools*, the things that do the recompiling. It doesn't go on the game.

| Thing | Name | Why |
|---|---|---|
| The repo / project | GBTG-Qilincomp | The game plus the toolchain that ports it |
| The recompiler CLI | `qilin` | The author's tool: short, unique, easy to type |
| C++ namespaces of the recompiler and runtime | `qilin`, `qilin::rt`, `qilin::gen` | Project namespace; avoids collisions with game, Zydis or system names |
| Build options, macros, CMake targets | `QILIN_*`, `qilin_*` | Same prefix as the namespace, so it's obvious what owns them |
| The game executable players run | `ghoulies` | Named after the game. Players launch Grabbed by the Ghoulies, not a tool |
| Settings file, save folder, window title | after the game | Same reason |
| Lifted functions | `fn_<address>` | Named by their address in the original XBE; real names come from analysis later |

New code follows the same rule: the prefix marks the toolchain, and anything a player sees is named after the game.
