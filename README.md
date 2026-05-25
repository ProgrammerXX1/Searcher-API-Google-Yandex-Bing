<p align="center">
  <h1 align="center">SearchX</h1>
  <p align="center">
    <b>High-performance C++ search aggregator</b><br/>
    <sub>Google + Yandex | Connection pooling | Sub-second latency</sub>
  </p>
  <p align="center">
    <a href="#performance"><img src="https://img.shields.io/badge/avg%20latency-556ms-brightgreen?style=flat-square" alt="latency"/></a>
    <a href="#build"><img src="https://img.shields.io/badge/C%2B%2B-20-blue?style=flat-square&logo=cplusplus" alt="C++20"/></a>
    <a href="#build"><img src="https://img.shields.io/badge/build-CMake-064F8C?style=flat-square&logo=cmake" alt="CMake"/></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-yellow?style=flat-square" alt="MIT"/></a>
    <a href="#api"><img src="https://img.shields.io/badge/API-REST%20JSON-orange?style=flat-square" alt="REST"/></a>
  </p>
</p>

---

## Overview

SearchX is a lightweight C++ search engine server that aggregates results from **Google** (via [Serper API](https://serper.dev)) and **Yandex** (via [Yandex Cloud Search API](https://cloud.yandex.ru/services/search-api)). It features connection pooling, parallel multi-engine queries, and a cyberpunk-styled web UI.

### Key Features

- **Connection pooling** &mdash; reuses TCP + TLS connections, eliminating DNS/handshake overhead
- **Parallel execution** &mdash; `std::async` runs multiple engines concurrently
- **Region-aware search** &mdash; Kazakhstan, Russia, or Global results via API-level geo targeting
- **Zero external runtime deps** &mdash; single static binary, links only to system libcurl
- **Auto-fetched build deps** &mdash; CMake `FetchContent` pulls everything at build time

---

## Performance

Benchmarked against equivalent Python (FastAPI + asyncio) implementation on identical queries:

| Metric | SearchX (C++) | Python | Speedup |
|:-------|:-------------|:-------|:--------|
| Google avg latency | **1294 ms** | 2411 ms | 1.9x |
| Yandex avg latency | **556 ms** | 2324 ms | **4.2x** |
| Google burst 10x | **1.58 s** | 5.37 s | **3.4x** |
| Yandex burst 10x | **1.64 s** | 3.09 s | 1.9x |
| Memory (RSS) | **43 MB** | 80 MB | 1.9x |

> Bottleneck is upstream API latency (Serper TTFB ~800ms, Yandex TTFB ~450ms). Connection pool eliminates ~300ms DNS+TCP+TLS overhead per request.

---

## Build

```bash
cd cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

**Requirements:**
- `g++ 13+` or `clang++ 16+`
- `cmake 3.20+`
- `libcurl4` (runtime only &mdash; headers are fetched automatically)

---

## Usage

```bash
# 1. Configure API keys
cp .env.example .env
vim .env

# 2. Run
./cpp/build/searchx --port 8000 --env .env --html index.html
```

Open [http://localhost:8000](http://localhost:8000)

---

## API

### `GET /api/search`

| Param | Type | Default | Description |
|:------|:-----|:--------|:------------|
| `q` | string | *required* | Search query |
| `engine_name` | string | `google` | `google` \| `yandex` \| `all` |
| `num` | int | `10` | Results count (1-30) |
| `region` | string | `kz` | `kz` \| `ru` \| `global` |

```bash
curl "http://localhost:8000/api/search?q=python&engine_name=all&num=10&region=kz"
```

```json
{
  "query": "python",
  "engine": "all",
  "region": "kz",
  "count": 15,
  "results": [
    { "title": "Welcome to Python.org", "url": "https://python.org/", "domain": "python.org", "snippet": "..." },
  ]
}
```

### `GET /api/regions`

### `GET /api/health`

---

## Architecture

```
                        +------------------+
                        |   Web Frontend   |
                        |   (index.html)   |
                        +--------+---------+
                                 |
                          GET /api/search
                                 |
                        +--------v---------+
                        |  cpp-httplib      |
                        |  HTTP Server      |
                        +--------+---------+
                                 |
                     +-----------+-----------+
                     |                       |
              +------v------+        +-------v-------+
              | Serper API  |        | Yandex Cloud  |
              | (Google)    |        | Search API    |
              +------+------+        +-------+-------+
                     |                       |
                  libcurl              libcurl
              (connection pool)    (connection pool)
```

---

## API Keys

| Service | Free tier | Link |
|:--------|:----------|:-----|
| Serper (Google) | 2,500 queries | [serper.dev](https://serper.dev) |
| Yandex Cloud Search | 1,000 queries/day | [console.yandex.cloud](https://console.yandex.cloud) |

---

## License

MIT
