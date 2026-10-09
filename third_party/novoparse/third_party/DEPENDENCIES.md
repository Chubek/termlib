# Native dependencies

NovoParse uses the checked-in `libglr` sources. Its minimal build also needs
these vendored components, which are included so builds require no network:

| Dependency | Upstream | Pinned commit | Included files |
| --- | --- | --- | --- |
| klib | https://github.com/attractivechaos/klib | `97a0fcb790b43b9e5da8994f4671021fec036f19` | arena allocator, threading, hash/vector/string headers, license |
| CTL | https://github.com/glouw/ctl | `435a2e6ba6cc4222e890b4c5993a750d29cb04e3` | `ctl.h`, `vec.h`, license |

Both use the MIT license. Their license files are retained beside their sources
under `libglr/third_party/`. The S-expression rewrite reader is the already
vendored sfsexp library; its licensing notices are retained in its directory.
