| Backend | Daemon ticks | Daemon + pinger ticks | CPU (% of one core) |
| --- | ---: | ---: | ---: |
| fping | 181 | 682 | 2.514 |
| fping-ts | 119 | 455 | 1.679 |
| native | 231 | 231 | 0.853 |
| native-ts | 174 | 174 | 0.640 |

| Backend | Phase | Added RTT p95 (ms) | Received probes >30 ms (%) | UL goodput (Mbit/s) | DL goodput (Mbit/s) |
| --- | --- | ---: | ---: | ---: | ---: |
| fping | upload-steady | 99.40 | 53.48 | 7.35 | — |
| fping | upload-step | 160.40 | 37.21 | 5.66 | — |
| fping | download-steady | 71.20 | 34.16 | — | 25.71 |
| fping | bidirectional | 105.40 | 46.77 | 6.67 | 13.00 |
| fping-ts | upload-steady | 75.10 | 23.26 | 6.81 | — |
| fping-ts | upload-step | 98.00 | 26.25 | 5.52 | — |
| fping-ts | download-steady | 42.70 | 20.40 | — | 30.83 |
| fping-ts | bidirectional | 79.00 | 44.11 | 6.61 | 26.88 |
| native | upload-steady | 100.70 | 54.73 | 7.38 | — |
| native | upload-step | 137.70 | 34.55 | 5.66 | — |
| native | download-steady | 75.70 | 50.83 | — | 31.05 |
| native | bidirectional | 105.70 | 46.84 | 6.93 | 12.37 |
| native-ts | upload-steady | 9.70 | 0.17 | 1.89 | — |
| native-ts | upload-step | 8.20 | 0.00 | 1.89 | — |
| native-ts | download-steady | 584.50 | 88.30 | — | 33.29 |
| native-ts | bidirectional | 592.50 | 88.14 | 1.00 | 32.82 |

| Backend | One-target detection (s) | All-target stall check (s) | Global timeout (s) | Reply after recovery (s) |
| --- | ---: | ---: | ---: | ---: |
| fping | 1.549 | 0.321 | 10.120 | 0.011 |
| fping-ts | 1.797 | 0.372 | 10.172 | 0.054 |
| native | 0.919 | 0.303 | 10.104 | 0.019 |
| native-ts | 1.864 | 0.245 | 10.044 | 0.035 |

| Native method | CPU lower | Health matched | Strict gate passes |
| --- | --- | --- | --- |
| native | True | True | False |
| native-ts | True | True | False |
