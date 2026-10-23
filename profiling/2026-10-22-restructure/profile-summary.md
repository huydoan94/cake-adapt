| Workload | Build | Runs | CPU ticks / 40 s, daemon + pinger (median) | of which pinger (median) | % of one core | filter runs | filter ns/run (median) | injector runs | injector ns/run (median) | RSS kB, daemon + pinger |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| idle | v0326-native | 3 | 19 / 19 / 16 (19) | 0 | 0.47 | 1075 | 1551 | 538 | 743 | 2360 |
| idle | vrestr-native | 3 | 29 / 18 / 19 (19) | 0 | 0.47 | 1078 | 2083 | 540 | 833 | 2360 |
| download | v0326-native | 3 | 41 / 30 / 31 (31) | 0 | 0.78 | 95861 | 2554 | 44978 | 280 | 2364 |
| download | vrestr-native | 3 | 50 / 37 / 32 (37) | 0 | 0.93 | 95036 | 3272 | 44645 | 396 | 2364 |
| upload | v0326-native | 3 | 23 / 22 / 21 (22) | 0 | 0.55 | 12404 | 4878 | 4396 | 588 | 2368 |
| upload | vrestr-native | 3 | 36 / 20 / 19 (20) | 0 | 0.50 | 12363 | 4979 | 4378 | 798 | 2364 |
| bidirectional | v0326-native | 3 | 34 / 30 / 29 (30) | 0 | 0.75 | 88471 | 2453 | 42786 | 282 | 2368 |
| bidirectional | vrestr-native | 3 | 48 / 36 / 32 (36) | 0 | 0.90 | 87004 | 2886 | 41777 | 401 | 2364 |
| congested | v0326-native | 3 | 37 / 34 / 29 (34) | 0 | 0.85 | 79146 | 2501 | 39082 | 328 | 2368 |
| congested | vrestr-native | 3 | 57 / 35 / 30 (35) | 0 | 0.88 | 76180 | 2811 | 37595 | 353 | 2364 |
| download6 | v0326-native | 3 | 58 / 37 / 30 (37) | 0 | 0.93 | 96062 | 3212 | 45003 | 767 | 2368 |
| download6 | vrestr-native | 3 | 49 / 27 / 31 (31) | 0 | 0.78 | 95176 | 2307 | 44909 | 504 | 2364 |
| bidirectional6 | v0326-native | 3 | 27 / 35 / 29 (29) | 0 | 0.72 | 86292 | 2275 | 41675 | 519 | 2364 |
| bidirectional6 | vrestr-native | 3 | 42 / 31 / 27 (31) | 0 | 0.78 | 85743 | 2487 | 41065 | 534 | 2360 |
