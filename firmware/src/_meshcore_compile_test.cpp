// Compile-only smoke test for MeshCore library integration.
// Verifies build machinery (lib paths, deps, defines) is correct.

#include <Arduino.h>

#if MOKI_USE_MESHCORE
  #include <Mesh.h>
  #include <Identity.h>
  #include <helpers/ESP32Board.h>
  #include <helpers/radiolib/CustomSX1262Wrapper.h>

  static void _moki_meshcore_compile_test() {
    // Force MeshCore symbols to be referenced so the linker pulls them in.
    // This is never called at runtime — just exists to validate the build.
    static ESP32Board test_board;
    (void)test_board.getStartupReason();
  }
#endif
