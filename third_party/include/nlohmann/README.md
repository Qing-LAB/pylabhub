# Vendored library: nlohmann/json

**Upstream:** https://github.com/nlohmann/json  
**Version:** v3.12.0  
**License:** MIT (see [LICENSE](LICENSE))  
**Vendored by:** Qing-LAB / pylabhub project  
**Date added:** 2025-10-30

## Purpose

This directory contains a vendored copy of the single-header JSON library
[nlohmann/json](https://github.com/nlohmann/json).  It provides a
stand-alone `json.hpp` used by the C++ components of pylabhub (e.g. FileLock,
JsonConfig).

Only the single header `json.hpp` and the corresponding license file are
included here to minimize repository size.  The file is taken directly from
the upstream release archive for the version listed above without modification.

If you need to upgrade or verify this copy:

```bash
# From repo root
cd cpp/third_party/include/nlohmann
wget https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp -O json.hpp

