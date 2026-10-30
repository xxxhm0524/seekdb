# Local FTS validation, 2026-10-30

Local Ubuntu/WSL, existing native release build, no AMD/cloud GPU use. Incremental builds used at most two concurrent compile tasks. No full clean build was performed. The retained production source under src/ matches 9f59c0b5e702e11e74f50fb95027b7be0b3fcdb7.

## Method

An exploratory 2,000-row run established that the environment worked. Formal-size local runs then used the unmodified benchmark: ROWS=20000, BATCH=500, ROUNDS=3000, QUERY_ROUNDS=200, SAMPLES=3, WARMUP=30, SKIP_LOAD=0. The MySQL client used the same local Unix socket for every variant. Every performance run recreated only the dedicated fts_large_bench database.

The saved pre-FTS-optimization executable was run in A/B/B/A order against the prior candidate (first parser changes plus child-completion reuse). Build times were essentially identical; the child-completion change was withdrawn. Two additional experimental runs and one corrected experimental run investigated stopword allocation/hash updates. This experiment was also withdrawn because the limited measurements did not establish a reliable aggregate gain. The retained first-parser candidate was incrementally rebuilt and run once as a final confirmation. This is not enough repetition for a statistically robust speedup claim.

## Retained candidate versus the two original baseline runs

The baseline column averages baseline-1 and baseline-2. The retained candidate column is one final run. Values and percentages are observational, not official scores; run order, warm-up and noise may account for small differences.

| Metric | Baseline mean | Retained | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 6.768000 | 6.973000 | -3.03% |
| build_ik_content_sec | 5.138500 | 5.140000 | -0.03% |
| build_beng_en_sec | 2.832500 | 2.833000 | -0.02% |
| tokenize_ik_avg_ms | 0.145200 | 0.143200 | +1.38% |
| tokenize_beng_avg_ms | 0.099300 | 0.098900 | +0.40% |
| query_cn_avg_ms | 1.295000 | 1.293000 | +0.15% |
| query_beng_avg_ms | 1.909750 | 1.902300 | +0.39% |
| query_mixed_avg_ms | 1.516100 | 1.482800 | +2.20% |
| query_limit_avg_ms | 1.677900 | 1.661900 | +0.95% |

## Correctness and limits

Complete TOKENIZE outputs and ordered document-ID lists for the four benchmark query patterns were captured, rather than comparing only hit counts. Retained-versus-baseline stdout and stderr match byte for byte. Coverage includes punctuation, mixed scripts, fullwidth text, repeated tokens, long inputs, embedded control characters, accent-insensitive equivalent keys and explicit collations.

Three statements return the same errors in both executables: NULL IK input with binary metadata (1235), the long repeated IK input (1210), and the latin1 BENG case (1583). These are matching baseline behaviors, not successful feature cases. This differential comparison is not a complete correctness proof or the official mysqltest suite.

Combined output SHA-256: a875e156b77f6366f0661a084ce9493700cc953e5723a46f32571280111126df.

Raw timings are retained in fts-local-results.json. The temporary benchmark directory is outside Git, so its reports contain git_head=unknown/git_dirty=1; those fields are not evidence of a clean source checkout. No checked-in benchmark/scoring input was changed.

The final source was compiled incrementally in the previously built compatible tree; unrelated submission changes were not clean-built. Cloud score, 50% improvement, and whole-engine regression freedom are not claimed. All test seekdb processes started by this validation were stopped gracefully afterward.

Resource scope: four bounded incremental rebuilds (roughly 16–45 seconds each), eight 20k-row performance runs in total including discarded experiments, one 2k-row exploratory run, no paid cloud resources. Full-size runs took roughly 22–23 seconds each on this machine. Raw logs and helper scripts remain in D:/CodexStorage/fts-validation, outside the repository.
