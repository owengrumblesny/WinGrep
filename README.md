# WinGrep

**Finally, a tool for finding text in files.** Nobody has ever tried this before.

WinGrep is GNU grep 3.9, dragged out of its natural Unix habitat and taught to speak to Windows directly, like an exchange student who was told the locals are friendly and then met `CreateFileW`.

## What this is

It is the actual GNU grep source code, compiled into a native `.exe`. It searches files, it uses regular expressions, and it behaves like grep, because it is grep.

## What this is not

- **A rewrite.** Nobody here is brave enough to rewrite a regex engine that decades of very serious people have been quietly perfecting. The hard part is theirs. The part where it runs on Windows is ours. We are aware of how that split looks.
- **A translation layer.** There is no Cygwin, no MSYS runtime, no WSL and no tiny Linux in a trench coat. It's one `.exe`. You can email it to someone, although you shouldn't, because it's 2026 and they will assume it's a virus.
- **Finished.** See the honesty section, which is the only section with no jokes in it. Sad, really.

## How it uses Windows APIs

Instead of pretending Windows is Unix and hoping it doesn't notice, the Unix-flavored calls are being replaced with real Windows ones in `win/compat.c`:

| Unix asks | Windows answers with | Bonus suffering |
|---|---|---|
| `sysconf (_SC_PAGESIZE)` | `GetSystemInfo` | It's 4096. It was always going to be 4096. |
| `isatty` | `GetConsoleMode` | The C runtime thinks `NUL` is a terminal. It's doing its best. |
| `is_device_mode` | `GetFileType` | Everything is a file, except when it's a character device, a pipe, or a personal attack. |
| `stat ("/dev/null")` | `NtQueryObject` | `/dev/null` doesn't exist here, so we ask the kernel for a device's name like a bouncer. |
| `fstat` | `GetFileInformationByHandle` | Directories are not allowed to be opened, which is fine and normal. |
| `openat` | `CreateFileW` + `_open_osfhandle` | Seven arguments to open a file. Six of them are for building character. |

## Build it

You need `gcc` from [MSYS2](https://www.msys2.org/) (the UCRT64 one) on your PATH. It's only the compiler; the finished exe doesn't need it.

```powershell
.\build.ps1
.\build\grep.exe --version
```

That's the whole build system. One PowerShell script. No `configure`, no `make`, no autotools, and no 30,000-line shell script checking whether your computer has a working `strlen`. It's a Windows machine. It will be fine.

`.\build.ps1 -Clean` rebuilds everything, for when you've lost faith.

## Use it

Exactly like grep, which is the entire pitch:

```powershell
.\build\grep.exe -rn "TODO" src
.\build\grep.exe -i "why is this here" notes.txt
```

## What's in here

| Folder | What it is |
|---|---|
| `src/` | GNU grep. The clever part. Don't touch it. (We touched it.) |
| `lib/` | The bits of gnulib grep still needs, including the regex engine. |
| `win/` | `compat.c`, where the Windows work lives, plus the `config.h` that `configure` generated once, before being retired with honors. |
| `build.ps1` | The entire build system. All of it. Really. |

## The honesty section

- This is not a from-scratch Windows port yet. Directory walking (`fts`), stack overflow handling, regex and plenty more still come from gnulib in `lib/`. The goal is replacing the Unix-shaped parts, not the regex engine.
- `-P` (Perl regex) isn't available. The build disables it.
- Without `-U`, a Ctrl-Z byte in a file stops reading there, because Windows text mode is older than most of its users. Use `-U` if that matters.
- Filenames and arguments outside your Windows code page aren't handled properly yet.
- Characters above U+FFFF, such as emoji, won't match as one character, because `wchar_t` is 16 bits on Windows.
- It's been compared against an earlier build on a set of 66 cases, not run through GNU's full test suite.
- This is a learning project. If it deletes your homework, it shouldn't, because grep doesn't write files. But still.

## License

GPLv3, see [LICENSE](LICENSE), because this is built directly from GNU grep and gnulib, and the GPL is the one thing in this repo that doesn't have a sense of humor.

All the hard parts are the work of the GNU grep and gnulib authors. Copyright notices remain in the source files. This version was modified in 2026 to call Windows APIs directly and to build without the GNU build system.
