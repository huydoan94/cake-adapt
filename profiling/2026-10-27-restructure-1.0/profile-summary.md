| Workload | Build | Runs | CPU ticks / 40 s, daemon + pinger (median) | of which pinger (median) | % of one core | filter runs | filter ns/run (median) | injector runs | injector ns/run (median) | RSS kB, daemon + pinger |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| idle | v0330-native | 3 | 18 / 18 / 23 (18) | 0 | 0.45 | 1076 | 1587 | 539 | 738 | 2360 |
| idle | vnew-native | 3 | 18 / 20 / 25 (20) | 0 | 0.50 | 1075 | 1461 | 538 | 830 | 2352 |
| download | v0330-native | 3 | 31 / 28 / 35 (31) | 0 | 0.78 | 95900 | 3072 | 44969 | 347 | 2364 |
| download | vnew-native | 3 | 31 / 32 / 44 (32) | 0 | 0.80 | 95048 | 3029 | 44371 | 345 | 2356 |
| upload | v0330-native | 3 | 22 / 22 / 19 (22) | 0 | 0.55 | 12378 | 5457 | 4377 | 570 | 2360 |
| upload | vnew-native | 3 | 24 / 22 / 24 (24) | 0 | 0.60 | 12580 | 5807 | 4457 | 649 | 2360 |
| bidirectional | v0330-native | 3 | 30 / 34 / 29 (30) | 0 | 0.75 | 87550 | 2921 | 41682 | 329 | 2364 |
| bidirectional | vnew-native | 3 | 37 / 49 / 34 (37) | 0 | 0.93 | 88081 | 3583 | 41966 | 408 | 2360 |
| congested | v0330-native | 3 | 31 / 39 / 63 (39) | 0 | 0.97 | 78338 | 3171 | 38477 | 367 | 2364 |
| congested | vnew-native | 3 | 37 / 32 / 31 (32) | 0 | 0.80 | 75633 | 3098 | 36988 | 398 | 2360 |
| download6 | v0330-native | 3 | 33 / 30 / 32 (32) | 0 | 0.80 | 95712 | 2901 | 45214 | 722 | 2364 |
| download6 | vnew-native | 3 | 37 / 35 / 39 (37) | 0 | 0.93 | 94853 | 3543 | 44221 | 834 | 2360 |
| bidirectional6 | v0330-native | 3 | 31 / 29 / 31 (31) | 0 | 0.78 | 88142 | 2926 | 42444 | 653 | 2360 |
| bidirectional6 | vnew-native | 3 | 37 / 36 / 32 (36) | 0 | 0.90 | 85302 | 3427 | 40788 | 772 | 2356 |
