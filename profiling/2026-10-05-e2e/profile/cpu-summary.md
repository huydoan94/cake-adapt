| Workload | Build | Runs | CPU ticks / 40 s (median) | % of one core | filter runs | filter ns/run (median) | RSS kB |
|---|---|---:|---|---:|---:|---:|---:|
| idle | base | 3 | 7 / 6 / 5 (6) | 0.15 | 1050 | 1537 | 2132 |
| idle | head | 3 | 5 / 5 / 4 (5) | 0.12 | 1055 | 1421 | 2112 |
| download | base | 3 | 25 / 15 / 12 (15) | 0.38 | 99353 | 2971 | 2136 |
| download | head | 3 | 9 / 12 / 10 (10) | 0.25 | 97617 | 1917 | 2112 |
| upload | base | 3 | 15 / 9 / 7 (9) | 0.23 | 12761 | 3261 | 2136 |
| upload | head | 3 | 9 / 9 / 10 (9) | 0.23 | 12594 | 3862 | 2116 |
| bidirectional | base | 3 | 43 / 16 / 11 (16) | 0.40 | 89677 | 2055 | 2140 |
| bidirectional | head | 3 | 16 / 15 / 15 (15) | 0.38 | 88688 | 2301 | 2120 |
| congested | base | 3 | 41 / 15 / 17 (17) | 0.42 | 76364 | 2386 | 2140 |
| congested | head | 3 | 15 / 14 / 12 (14) | 0.35 | 75518 | 1876 | 2116 |
