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
- **Zero external runtime deps** &mdash; single binary, links only to system libcurl
- **Auto-fetched build deps** &mdash; CMake `FetchContent` pulls everything at build time

---

## Performance

Benchmarked on 40 queries (20 RU + 20 EN), sequential & parallel:

| Metric | Value |
|:-------|:------|
| Google avg latency | **1294 ms** |
| Google median | **1231 ms** |
| Google min | **659 ms** |
| Yandex avg latency | **556 ms** |
| Yandex median | **537 ms** |
| Yandex min | **431 ms** |
| Burst 10 concurrent (Google) | **1.58 s total** |
| Burst 10 concurrent (Yandex) | **1.64 s total** |
| Burst 20 concurrent (Google) | **2.79 s total** |
| Memory (RSS) | **43 MB** |

### Latency breakdown (per request)

```
Request lifecycle with connection pool (warm):

  DNS resolve ........... 0 ms   (cached)
  TCP connect ........... 0 ms   (reused)
  TLS handshake ......... 0 ms   (reused)
  ----------------------------------------
  Serper API TTFB ....... 800-1200 ms   (upstream, not optimizable)
  Yandex API TTFB ....... 430-520 ms    (upstream, not optimizable)
  ----------------------------------------
  JSON/XML parse ........ <1 ms
  HTTP response ......... <1 ms
```

### Why latency can't go lower

The measured latency is **at the physical limit** for these APIs:

| Factor | Impact | Can we optimize? |
|:-------|:-------|:-----------------|
| **Upstream API processing time** | Serper: 800-1200ms, Yandex: 430-520ms | No &mdash; server-side, out of our control |
| **Network round-trip** | ~60-80ms to API servers | No &mdash; depends on geographic distance |
| **DNS resolution** | ~150ms first request, then 0ms | Already eliminated via connection pool |
| **TCP + TLS handshake** | ~250ms first request, then 0ms | Already eliminated via connection pool |
| **Local processing** (JSON/XML parse, regex) | <1ms | Already negligible |

> **Bottom line:** SearchX adds **<5ms overhead** on top of raw API latency. The only way to go faster is to move the server closer to the API endpoints (e.g. deploy in US for Serper, in Russia for Yandex) or switch to faster upstream APIs.

### Cold start vs warm connection

| | Cold (first request) | Warm (pooled) |
|:--|:-----|:------|
| Serper | ~1900 ms | **~900 ms** |
| Yandex | ~740 ms | **~470 ms** |

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
    { "title": "Welcome to Python.org", "url": "https://python.org/", "domain": "python.org", "snippet": "..." }
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
