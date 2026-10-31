#!/usr/bin/env python3
"""Paired end-to-end run: fresh seekdb, load data, time CREATE VECTOR INDEX and
the first query separately, then 4096 Top-10 and Top-100 single-row APPROXIMATE
queries. Validates returned ids and the bridge actually mapped by the process.

usage: e2e_pair.py <dataset_dir> <dim> <variant> [observer_binary]
Dataset dir needs insert.tsv, queries.txt, gt.i32 (nq x 100 int32, dataset ids).
"""
import glob
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time

ROOT = os.environ.get("SEEKDB_BENCH_ROOT", "/root/amd-score-20261030")
OBSERVER = ROOT + "/src/build_release/src/observer/seekdb"
BRIDGE = ROOT + "/bridge/libseekdb_cuvs_bridge.so"
VARIANTS = ROOT + "/bridge/variants"
RUNROOT = ROOT + "/run"
RESROOT = ROOT + "/results"
PORT = 2881
NQ = 4096
K = 100

def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


dataset, dim, variant = sys.argv[1], int(sys.argv[2]), sys.argv[3]
binary = sys.argv[4] if len(sys.argv) > 4 else OBSERVER
run_order = sys.argv[5] if len(sys.argv) > 5 else os.environ.get("RUN_ORDER", "")
bin_tag = os.path.basename(binary)
name = os.path.basename(dataset.rstrip("/"))
run = os.path.join(RUNROOT, f"{name}_{variant}_{bin_tag}" + (f"_{run_order}" if run_order else ""))
shutil.rmtree(run, ignore_errors=True)
os.makedirs(run)
os.makedirs(RESROOT, exist_ok=True)
src_so = os.path.join(VARIANTS, variant + ".so")
shutil.copyfile(src_so, BRIDGE)
so_md5 = md5_of(BRIDGE)
src_md5 = md5_of(src_so)
so_ino = os.stat(BRIDGE).st_ino
bin_md5 = md5_of(binary)
src_c = os.path.join(VARIANTS, variant + ".c")
src_c_md5 = md5_of(src_c) if os.path.exists(src_c) else None

trace = os.path.join(run, "trace.log")
log = os.path.join(run, "observer.log")
env = os.environ.copy()
env["LD_LIBRARY_PATH"] = ROOT + "/bridge:/opt/hipvs/lib:/opt/rocm/lib"
env["OB_VSAG_TRACE"] = "1"
env["OB_VSAG_TRACE_FILE"] = trace
env.pop("OB_VSAG_USE_CUVS", None)

cmd = [binary, "--nodaemon", "--port", str(PORT),
       "--data-dir", os.path.join(run, "store"), "--base-dir", run,
       "--parameter", "__min_full_resource_pool_memory=2147483648",
       "--parameter", "memory_budget=12G", "--parameter", "datafile_size=4G",
       "--parameter", "cpu_count=24", "--parameter", "local_ip=127.0.0.1"]
logf = open(log, "wb")
proc = subprocess.Popen(cmd, env=env, stdout=logf, stderr=subprocess.STDOUT)


def mysql(sql, timeout=3600):
    c = ["mysql", "--connect-timeout=5", "--protocol=TCP", "-h127.0.0.1",
         f"-P{PORT}", "-uroot", "-A", "-N", "-B", "--local-infile=1"]
    return subprocess.run(c, input="set ob_query_timeout=300000000;\n" + sql,
                          text=True, capture_output=True, timeout=timeout)


def stop():
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(60)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(10)
    logf.close()


vram_paths = glob.glob("/sys/class/drm/card*/device/mem_info_vram_used")
vram_peak = [0]
vram_proc_peak = [0]
rss_peak = [0]
sampling = [True]


def proc_rss(pid):
    try:
        for line in open(f"/proc/{pid}/status"):
            if line.startswith("VmHWM:"):
                return int(line.split()[1]) * 1024
    except OSError:
        pass
    return 0


def proc_cpu_seconds(pid):
    try:
        parts = open(f"/proc/{pid}/stat").read().rsplit(") ", 1)[1].split()
        return (int(parts[11]) + int(parts[12])) / os.sysconf("SC_CLK_TCK")
    except (OSError, IndexError, ValueError):
        return None


def proc_vram(pid):
    total = 0
    for fd in glob.glob(f"/proc/{pid}/fdinfo/*"):
        try:
            txt = open(fd).read()
        except OSError:
            continue
        for line in txt.splitlines():
            if line.startswith("drm-memory-vram") or line.startswith("drm-total-vram"):
                try:
                    total = max(total, int(line.split(":")[1].strip().split()[0]) * 1024)
                except (ValueError, IndexError):
                    pass
    return total


def sample_vram():
    while sampling[0]:
        cur = 0
        for p in vram_paths:
            try:
                cur = max(cur, int(open(p).read().strip()))
            except OSError:
                pass
        vram_peak[0] = max(vram_peak[0], cur)
        if proc.poll() is None:
            vram_proc_peak[0] = max(vram_proc_peak[0], proc_vram(proc.pid))
            rss_peak[0] = max(rss_peak[0], proc_rss(proc.pid))
        time.sleep(0.2)


gt = struct.unpack("<%di" % (NQ * K), open(os.path.join(dataset, "gt.i32"), "rb").read())


def recall_at(got, k):
    hits = 0
    for i in range(NQ):
        truth = set(gt[i * K:i * K + k])
        hits += len(set(got[i][:k]) & truth)
    return hits / (NQ * k)


def parse_ids(out, k):
    vals = [int(x) for x in out.split()]
    if len(vals) != NQ * k:
        raise RuntimeError(f"expected {NQ*k} ids, got {len(vals)}")
    return [vals[i * k:(i + 1) * k] for i in range(NQ)]


def check_ids(got, k, valid):
    dup = 0
    bad = 0
    for row in got:
        if len(set(row[:k])) != k:
            dup += 1
        if any(v not in valid for v in row[:k]):
            bad += 1
    if dup or bad:
        raise RuntimeError(f"bad result rows: duplicate={dup} invalid_id={bad}")


valid_ids = set()
with open(os.path.join(dataset, "insert.tsv")) as f:
    for line in f:
        tab = line.find("\t")
        if tab > 0:
            valid_ids.add(int(line[:tab]))

try:
    t_start = time.time()
    deadline = t_start + 300
    while True:
        if proc.poll() is not None:
            raise RuntimeError("observer exited during startup")
        if time.time() > deadline:
            raise RuntimeError("observer did not become SQL-ready")
        if mysql("select 1", timeout=10).returncode == 0:
            break
        time.sleep(0.5)
    ready_s = time.time() - t_start

    maps_ok = False
    mapped_ino = None
    mapped_lines = []
    try:
        for line in open(f"/proc/{proc.pid}/maps"):
            if "seekdb_cuvs_bridge" in line:
                mapped_lines.append(line.rstrip())
                if BRIDGE in line:
                    maps_ok = True
                parts = line.split()
                if len(parts) >= 5:
                    mapped_ino = int(parts[4])
    except OSError:
        pass
    if not maps_ok:
        raise RuntimeError("bridge .so not mapped by observer")
    if mapped_ino is not None and mapped_ino != so_ino:
        raise RuntimeError(f"mapped bridge inode {mapped_ino} != copied {so_ino}")

    r = mysql("alter system set ob_vector_memory_limit_percentage=30;")
    if r.returncode != 0:
        raise RuntimeError("alter system failed: " + r.stderr)
    r = mysql("create database if not exists bench;")
    if r.returncode != 0:
        raise RuntimeError("create db failed: " + r.stderr)

    t_load = time.monotonic()
    r = mysql(f"use bench; drop table if exists items; create table items(id bigint primary key, v vector({dim}));")
    if r.returncode != 0:
        raise RuntimeError("create table failed: " + r.stderr)
    r = mysql(f"use bench; load data local infile '{dataset}/insert.tsv' into table items "
              "fields terminated by '\\t' lines terminated by '\\n' (id,v);", timeout=7200)
    if r.returncode != 0:
        raise RuntimeError("load data failed: " + r.stderr)
    r = mysql("use bench; select count(*) from items;")
    count = int(r.stdout.strip())
    if count != 250000:
        raise RuntimeError(f"row count {count}")
    load_s = time.monotonic() - t_load

    queries = [l.strip() for l in open(os.path.join(dataset, "queries.txt")) if l.strip()]
    if len(queries) != NQ:
        raise RuntimeError(f"query count {len(queries)}")

    sampler = threading.Thread(target=sample_vram, daemon=True)
    sampler.start()

    ddl_sql = ("use bench; create vector index idx on items(v) with "
               "(distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64);")
    first_q = ("use bench; select id from items order by l2_distance(v,'" + queries[0] +
               "') approximate limit 10;")
    ddl_start_epoch = time.time()
    t0 = time.monotonic()
    r = mysql(ddl_sql, timeout=3600)
    ddl_only_s = time.monotonic() - t0
    ddl_end_epoch = time.time()
    if r.returncode != 0:
        raise RuntimeError("create index failed: " + r.stderr)
    t0 = time.monotonic()
    r = mysql(first_q, timeout=600)
    first_q_s = time.monotonic() - t0
    if r.returncode != 0:
        raise RuntimeError("first query failed: " + r.stderr)
    first_vals = [int(x) for x in r.stdout.split()]
    if len(first_vals) != 10 or len(set(first_vals)) != 10 or any(v not in valid_ids for v in first_vals):
        raise RuntimeError(f"first query invalid: {first_vals}")
    ddl_s = ddl_only_s + first_q_s

    warm = "use bench;\n" + "".join(
        "select id from items order by l2_distance(v,'" + q + "') approximate limit 10;\n"
        for q in queries[:64])
    r = mysql(warm, timeout=1800)
    if r.returncode != 0:
        raise RuntimeError("warmup failed: " + r.stderr)

    script10 = "use bench;\n" + "".join(
        "select id from items order by l2_distance(v,'" + q + "') approximate limit 10;\n"
        for q in queries)
    t0 = time.monotonic()
    r = mysql(script10, timeout=3600)
    q10_s = time.monotonic() - t0
    if r.returncode != 0:
        raise RuntimeError("top10 failed: " + r.stderr)
    got10 = parse_ids(r.stdout, 10)
    check_ids(got10, 10, valid_ids)
    rec10 = recall_at(got10, 10)

    script100 = "use bench;\n" + "".join(
        "select id from items order by l2_distance(v,'" + q + "') approximate limit 100;\n"
        for q in queries)
    t0 = time.monotonic()
    r = mysql(script100, timeout=3600)
    q100_s = time.monotonic() - t0
    if r.returncode != 0:
        raise RuntimeError("top100 failed: " + r.stderr)
    got100 = parse_ids(r.stdout, 100)
    check_ids(got100, 100, valid_ids)
    rec100 = recall_at(got100, 100)

    cpu_s = proc_cpu_seconds(proc.pid)
    rss_gb = rss_peak[0] / 1e9
    sampling[0] = False
    sampler.join(timeout=2)

    snapshot_meta = None
    build_us = graph_bytes = meta_us = stream_us = add_span_us = None
    gpu_build_us = tmp_ser_us = None
    bridge_params = None
    seekdb_log = os.path.join(run, "log", "seekdb.log")
    try:
        for line in open(seekdb_log, errors="replace"):
            if "[seekdb_cuvs] build params" in line:
                bridge_params = line.split("build params", 1)[1].strip()
            if "serialized GPU CAGRA snapshot" in line:
                def g(pat):
                    m = re.search(pat + r"=(\d+)", line)
                    return int(m.group(1)) if m else None
                gpu_build_us = g("gpu_build_us")
                tmp_ser_us = g("tmp_ser_us")
                build_us = gpu_build_us
                meta_us = g("meta_us")
                stream_us = g("stream_us")
                add_span_us = g("add_span_us")
                graph_bytes = g("graph_bytes")
                mb = re.search(r"built=(\w+)", line)
                snapshot_meta = dict(built=(mb.group(1) if mb else None),
                                     gpu_build_us=gpu_build_us, tmp_ser_us=tmp_ser_us,
                                     meta_us=meta_us, stream_us=stream_us,
                                     add_span_us=add_span_us, graph_bytes=graph_bytes)
    except OSError:
        pass

    if snapshot_meta is None or snapshot_meta['built'] != 'true':
        raise RuntimeError('GPU build snapshot evidence missing')
    combined_log = open(seekdb_log, errors='replace').read() + open(log, errors='replace').read()
    if any(x in combined_log for x in ('lazily built VSAG fallback', '[seekdb_cuvs] search rc=', 'GPU CAGRA build failed')):
        raise RuntimeError('GPU failure or CPU fallback detected')
    res = dict(dataset=name, dim=dim, variant=variant, binary=bin_tag,
               run_order=run_order, binary_path=binary, binary_md5=bin_md5,
               bridge_md5=so_md5, bridge_src_so_md5=src_md5,
               bridge_src_c_md5=src_c_md5, bridge_params=bridge_params,
               bridge_mapped=True, mapped_lines=mapped_lines[:4],
               ddl_sql=ddl_sql, nq=NQ, k=K, snapshot_meta=snapshot_meta,
               rows=count,
               ddl_seconds=round(ddl_s, 3), ddl_only_seconds=round(ddl_only_s, 3),
               ddl_start_epoch=round(ddl_start_epoch, 3),
               ddl_end_epoch=round(ddl_end_epoch, 3),
               first_query_seconds=round(first_q_s, 3),
               gpu_build_us=gpu_build_us, tmp_ser_us=tmp_ser_us,
               nonbuild_seconds=(round(ddl_only_s - (gpu_build_us + (tmp_ser_us or 0)) / 1e6, 3)
                                 if gpu_build_us else None),
               graph_bytes=graph_bytes,
               first_query_rows=len(first_vals),
               qps10=round(NQ / q10_s, 2), qps100=round(NQ / q100_s, 2),
               recall10=round(rec10, 4), recall100=round(rec100, 4),
               vram_peak_gb=round(vram_peak[0] / 1e9, 2),
               vram_proc_peak_gb=(round(vram_proc_peak[0] / 1e9, 2) or None),
               rss_peak_gb=round(rss_gb, 2), cpu_seconds=(round(cpu_s, 1) if cpu_s else None),
               load_seconds=round(load_s, 1), ready_seconds=round(ready_s, 1))
    suffix = f"_{run_order}" if run_order else ""
    with open(os.path.join(RESROOT, f"{name}_{variant}_{bin_tag}{suffix}.json"), "w") as f:
        json.dump(res, f, indent=2)
    print("RESULT " + json.dumps(res), flush=True)
finally:
    sampling[0] = False
    stop()
    store_path = os.path.realpath(os.path.join(run, 'store'))
    if store_path.startswith(os.path.realpath(RUNROOT) + os.sep):
        shutil.rmtree(store_path, ignore_errors=True)
