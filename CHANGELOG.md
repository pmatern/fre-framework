# Changelog

All notable changes to fre-framework will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Added
- Initial project structure: CMakeLists.txt, CMakePresets.json, directory tree
- cmake/dependencies.cmake with CPM.cmake bootstrap and all declared dependencies
- cmake/FreConfig.cmake.in for `find_package(fre)` consumer support
- cmake/version.hpp.in for `fre/version.hpp` generation
- .clang-format and .clang-tidy configuration
- tests/CMakeLists.txt with `fre_add_test` helper function
- CHANGELOG.md (this file)

---

<!-- Links — updated with each release tag -->
[Unreleased]: https://github.com/your-org/fre-framework/compare/HEAD...HEAD
