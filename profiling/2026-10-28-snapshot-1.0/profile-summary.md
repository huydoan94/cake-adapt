| Workload | Build | Runs | CPU ticks / 40 s, daemon + pinger (median) | of which pinger (median) | % of one core | filter runs | filter ns/run (median) | injector runs | injector ns/run (median) | RSS kB, daemon + pinger |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| idle | v0330-native | 3 | 28 / 19 / 19 (19) | 0 | 0.47 | 1076 | 935 | 539 | 751 | 2688 |
| idle | v100-native | 3 | 33 / 20 / 19 (20) | 0 | 0.50 | 1075 | 890 | 539 | 859 | 2684 |
| download | v0330-native | 3 | 32 / 22 / 19 (22) | 0 | 0.55 | 79723 | 1781 | 38447 | 293 | 2692 |
| download | v100-native | 3 | 32 / 25 / 25 (25) | 0 | 0.62 | 93178 | 1760 | 44808 | 276 | 2696 |
| upload | v0330-native | 3 | 23 / 17 / 19 (19) | 0 | 0.47 | 12572 | 3101 | 4564 | 493 | 2696 |
| upload | v100-native | 3 | 24 / 19 / 19 (19) | 0 | 0.47 | 12477 | 2899 | 4532 | 455 | 2696 |
| bidirectional | v0330-native | 3 | 48 / 26 / 25 (26) | 0 | 0.65 | 83709 | 1937 | 41727 | 316 | 2696 |
| bidirectional | v100-native | 3 | 34 / 26 / 26 (26) | 0 | 0.65 | 87440 | 1797 | 43756 | 296 | 2696 |
| congested | v0330-native | 3 | 31 / 26 / 23 (26) | 0 | 0.65 | 71425 | 1773 | 36693 | 290 | 2696 |
| congested | v100-native | 3 | 32 / 26 / 27 (27) | 0 | 0.68 | 74046 | 1765 | 37773 | 271 | 2696 |
| download6 | v0330-native | 3 | 27 / 25 / 26 (26) | 0 | 0.65 | 93244 | 1860 | 45000 | 352 | 2696 |
| download6 | v100-native | 3 | 33 / 24 / 26 (26) | 0 | 0.65 | 89825 | 1878 | 43456 | 350 | 2696 |
| bidirectional6 | v0330-native | 3 | 27 / 25 / 26 (26) | 0 | 0.65 | 81822 | 1863 | 40441 | 377 | 2692 |
| bidirectional6 | v100-native | 3 | 36 / 25 / 27 (27) | 0 | 0.68 | 88701 | 1742 | 44127 | 352 | 2692 |
