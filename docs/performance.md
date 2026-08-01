| Threads | Scenario             | Tasks  | Time    | Throughput       |
|---------|----------------------|--------|---------|------------------|
| 1       | External submission  | 100000 | 0.0483s | 2069313 tasks/s  |
| 2       | External submission  | 100000 | 0.0998s | 1002183 tasks/s  |
| 4       | External submission  | 100000 | 0.1112s | 899416 tasks/s   |
| 8       | External submission  | 100000 | 0.2577s | 388103 tasks/s   |
| 1       | Nested work stealing | 100000 | 0.0123s | 8146419 tasks/s  |
| 2       | Nested work stealing | 100000 | 0.0246s | 4062433 tasks/s  |
| 4       | Nested work stealing | 100000 | 0.0612s | 1633424 tasks/s  |
| 8       | Nested work stealing | 100000 | 0.2108s | 474363 tasks/s |