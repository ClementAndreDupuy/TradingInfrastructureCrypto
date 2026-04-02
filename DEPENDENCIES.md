# Dependencies

## C++ Libraries

| Library | Purpose |
|---|---|
| **nlohmann_json** | JSON parsing/serialization |
| **libwebsockets** | WebSocket connections for live feeds |
| **CURL** | HTTP REST API calls |
| **OpenSSL** | TLS, HMAC, ECDSA signing, SHA hashing |
| **pybind11** | C++/Python bindings |
| **Google Test (GTest)** | Unit testing |

Build tooling: **CMake >= 3.15**, **PkgConfig**

## Python Libraries

| Library | Version | Purpose |
|---|---|---|
| **numpy** | >= 1.24 | Numerical computing |
| **torch** | - | PyTorch deep learning |
| **polars** | - | Fast dataframes |
| **scipy** | - | Scientific computing (`scipy.stats`) |
| **requests** | - | HTTP client |
| **pyyaml** | - | YAML config parsing |
| **pytest** | - | Testing |
| **pybind11** | >= 2.11 | C++/Python bindings |
| **scikit-build-core** | >= 0.9 | Build system for C++ extensions |
