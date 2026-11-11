# Vampires vs Werewolves — updated C++ client package

This package contains only the updated C++ client project. Place `cpp_client/`
inside the existing repository. Do not move the professor-provided server or
Map Generator files into `cpp_client/`.

Recommended repository layout:

```text
VampiresvsWerewolvesAI/
├── vvws_cpp_client/
│   ├── CMakeLists.txt
│   ├── client.cpp
│   ├── README.md
│   ├── cmake/toolchains/mingw-w64-x86_64.cmake
│   └── tests/mock_server_test.py
└── game_server/
    ├── Server v3.2.1/
    └── Map Generator/
```

See `cpp_client/README.md` for macOS development, Windows cross-compilation,
and mock-server testing commands.
