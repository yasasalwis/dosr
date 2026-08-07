# lib/

Private libraries for this project. Empty today — every module is a plain
`.cpp`/`.h` pair under `src/` and `include/`, which is the right shape while
they all share the same globals (`cfg`, `pumps`, `sensors`, `control`).

Pull something out into a library here when it becomes genuinely standalone,
for example a pH/EC conversion module you would want to unit-test on the host
without Arduino. One directory per library:

```
lib/
└── ec_math/
    ├── library.json      (optional; declares deps and metadata)
    ├── ec_math.h
    └── ec_math.cpp
```

PlatformIO's Library Dependency Finder scans `#include` directives, compiles
each of these into its own static archive, and links only what is used. Nothing
needs to be added to `platformio.ini`.

External libraries do *not* belong here — declare them under `lib_deps` in
`platformio.ini` so versions are pinned and reproducible.

More: https://docs.platformio.org/page/librarymanager/ldf.html
