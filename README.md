# OySyn Search Engine (C++)

High-performance search aggregator combining Google (via Serper API) and Yandex (via Yandex Cloud Search API) with connection pooling and parallel execution.

## Performance

| Metric | C++ (this) | Python |
|--------|-----------|--------|
| Google avg | **1294ms** | 2411ms |
| Yandex avg | **556ms** | 2324ms |
| Burst 10x Google | **1.58s** | 5.37s |
| RAM | **43 MB** | 80 MB |

## Build

```bash
cd cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

**Requirements:** g++ 13+, cmake 3.20+, libcurl4 (runtime). All other dependencies are fetched automatically.

## Run

```bash
cp .env.example .env
# Edit .env with your API keys

./cpp/build/oysyn_search --port 8000 --env .env --html index.html
```

Open http://localhost:8000

## API

```
GET /api/search?q=python&engine_name=google&num=10&region=kz
GET /api/search?q=python&engine_name=yandex&num=10&region=ru
GET /api/search?q=python&engine_name=all&num=10&region=kz
GET /api/regions
GET /api/health
```

**Engines:** `google`, `yandex`, `all`
**Regions:** `kz` (Kazakhstan), `ru` (Russia), `global`

## API Keys

- **Serper** (Google): https://serper.dev — 2500 free queries
- **Yandex Cloud Search API**: https://console.yandex.cloud — 1000 free queries/day

## Architecture

- **HTTP Server:** cpp-httplib (header-only)
- **HTTP Client:** libcurl with connection pooling (reuses TCP+TLS)
- **JSON:** nlohmann/json
- **XML:** pugixml (for Yandex API response parsing)
- **Concurrency:** `std::async` for parallel multi-engine search
