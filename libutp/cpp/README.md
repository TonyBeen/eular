# C++ Implementation

The production C++ implementation lives here:

- `include/`: installed C++ library headers
- `src/`: implementation and private headers
- `test/`: C++ unit, integration, and fault-injection tests
- `examples/`: C++ example programs

The repository root retains shared build configuration and third-party code in
`3rd/`. The root CMake build continues to configure the C++ target; the C
migration workspace remains independent in `c/`.

The C++ subtree can also be configured directly while reusing the shared root:

```sh
cmake -S cpp -B build/cpp -DUTP_BUILD_TESTS=ON -DUTP_BUILD_LSQUIC_ECHO=OFF
cmake --build build/cpp --parallel
ctest --test-dir build/cpp --output-on-failure
```
