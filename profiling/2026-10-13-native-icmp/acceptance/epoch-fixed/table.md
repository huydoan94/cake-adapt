| Backend | Daemon ticks | Daemon + pinger ticks | CPU (% of one core) |
| --- | ---: | ---: | ---: |
| fping-ts | 114 | 429 | 1.583 |
| native-ts | 268 | 268 | 0.988 |

| Backend | Phase | Added RTT p95 (ms) | Received probes >30 ms (%) | UL goodput (Mbit/s) | DL goodput (Mbit/s) |
| --- | --- | ---: | ---: | ---: | ---: |
| fping-ts | upload-steady | 76.30 | 25.42 | 6.95 | — |
| fping-ts | upload-step | 91.40 | 24.25 | 5.55 | — |
| fping-ts | download-steady | 42.20 | 23.42 | — | 32.14 |
| fping-ts | bidirectional | 74.20 | 45.35 | 6.62 | 26.70 |
| native-ts | upload-steady | 78.70 | 25.29 | 6.88 | — |
| native-ts | upload-step | 98.70 | 26.41 | 5.57 | — |
| native-ts | download-steady | 40.90 | 21.39 | — | 32.21 |
| native-ts | bidirectional | 69.80 | 44.61 | 6.59 | 28.68 |

| Backend | One-target detection (s) | All-target stall check (s) | Global timeout (s) | Reply after recovery (s) |
| --- | ---: | ---: | ---: | ---: |
| fping-ts | 0.851 | 0.423 | 10.027 | 0.036 |
| native-ts | 1.712 | 0.262 | 10.063 | 0.044 |

| Native method | CPU lower | Health matched | Strict gate passes |
| --- | --- | --- | --- |
| native-ts | True | True | False |
