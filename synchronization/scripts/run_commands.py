#!/usr/bin/env python3
import argparse
import csv
import json
import os
import shlex
import subprocess
import sys
import threading
import time
from typing import List, Dict

DEFAULT_SSH_OPTS = [
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=accept-new",
]


def now_utc_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def load_plan(path: str, bin_root: str = "") -> List[List[str]]:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, list):
        raise ValueError("plan must be a JSON list of command lists")
    plan: List[List[str]] = []
    for group in data:
        if not isinstance(group, list):
            raise ValueError("each group must be a JSON list of commands")
        resolved: List[str] = []
        for cmd in group:
            if not isinstance(cmd, str) or not cmd.strip():
                raise ValueError("commands must be non-empty strings")
            if "{BIN_ROOT}" in cmd:
                if not bin_root:
                    raise ValueError(
                        f"plan contains {{BIN_ROOT}} but --bin_root not set: {cmd!r}"
                    )
                cmd = cmd.replace("{BIN_ROOT}", bin_root)
            resolved.append(cmd)
        plan.append(resolved)
    return plan


def ensure_csv_header(path: str, header: List[str]) -> None:
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(header)


def truncate(s: str, max_len: int) -> str:
    if max_len <= 0:
        return s
    if len(s) <= max_len:
        return s
    return s[:max_len] + "...<truncated>"


def run_ssh(host: str, cmd: str, ssh_opts: List[str], timeout: int,
            debug: bool, direct: bool) -> Dict[str, str]:
    start = time.time()
    if direct:
        argv = shlex.split(cmd)
        full_cmd = ["ssh"] + ssh_opts + [host, "--"] + argv
    else:
        full_cmd = ["ssh"] + ssh_opts + [host, "--", "bash", "-lc", cmd]
    if debug:
        quoted = " ".join(shlex.quote(s) for s in full_cmd)
        print(f"[debug] {quoted}")
        print(f"[debug] remote_cmd repr={cmd!r}")
    try:
        proc = subprocess.run(
            full_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
        rc = proc.returncode
        out = proc.stdout
        err = proc.stderr
    except subprocess.TimeoutExpired as e:
        rc = 124
        out = e.stdout or ""
        err = e.stderr or ""
        if isinstance(out, bytes):
            out = out.decode(errors="replace")
        if isinstance(err, bytes):
            err = err.decode(errors="replace")
        err = err + "\nTIMEOUT"
    except Exception as e:  # pragma: no cover - unexpected
        rc = 125
        out = ""
        err = f"runner error: {e}"
    end = time.time()
    return {
        "host": host,
        "command": cmd,
        "exit_code": str(rc),
        "duration_s": f"{end - start:.3f}",
        "stdout": out,
        "stderr": err,
    }


def run_command_all(hosts: List[str], cmd: str, ssh_opts: List[str], timeout: int,
                    debug: bool, direct: bool) -> List[Dict[str, str]]:
    results: List[Dict[str, str]] = []
    lock = threading.Lock()
    cmd = cmd.replace("\r", "").strip()

    def worker(h: str, idx: int) -> None:
        formatted = cmd.replace("{host}", h).replace("{host_idx}", str(idx))
        res = run_ssh(h, formatted, ssh_opts, timeout, debug, direct)
        with lock:
            results.append(res)

    threads = [threading.Thread(target=worker, args=(h, idx)) for idx, h in enumerate(hosts)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return results


def is_initializer_cmd(cmd: str) -> bool:
    try:
        tokens = shlex.split(cmd)
    except ValueError:
        return "--init" in cmd or "--daemon" in cmd
    for t in tokens:
        if t == "--init" or t == "--daemon":
            return True
        if t.startswith("--init=") or t.startswith("--daemon="):
            return True
    return False


def extract_hosts_count(cmd: str) -> int | None:
    try:
        tokens = shlex.split(cmd)
    except ValueError:
        tokens = cmd.split()
    for i, tok in enumerate(tokens):
        if tok == "--hosts" and i + 1 < len(tokens):
            try:
                return int(tokens[i + 1])
            except ValueError:
                return None
        if tok.startswith("--hosts="):
            try:
                return int(tok.split("=", 1)[1])
            except ValueError:
                return None
    return None


def run_command_per_host(hosts: List[str], cmds: List[str], ssh_opts: List[str],
                         timeout: int, non_init_delay: float, debug: bool,
                         direct: bool) -> List[Dict[str, str]]:
    results: List[Dict[str, str]] = []
    lock = threading.Lock()

    cmds = [c.replace("\r", "").strip() for c in cmds]
    init_indices = [i for i, c in enumerate(cmds) if is_initializer_cmd(c)]

    def worker(h: str, idx: int, cmd: str) -> None:
        formatted = cmd.replace("{host}", h).replace("{host_idx}", str(idx))
        res = run_ssh(h, formatted, ssh_opts, timeout, debug, direct)
        with lock:
            results.append(res)

    threads = []
    pending = []
    for idx, (h, cmd) in enumerate(zip(hosts, cmds)):
        if init_indices and idx in init_indices:
            t = threading.Thread(target=worker, args=(h, idx, cmd))
            threads.append(t)
        else:
            pending.append((h, idx, cmd))
    for t in threads:
        t.start()
    if init_indices and non_init_delay > 0:
        if debug:
            print(f"[debug] delaying non-init hosts by {non_init_delay}s")
        time.sleep(non_init_delay)
    for h, idx, cmd in pending:
        t = threading.Thread(target=worker, args=(h, idx, cmd))
        threads.append(t)
        t.start()
    for t in threads:
        t.join()
    return results


def main() -> int:
    ap = argparse.ArgumentParser(description="Run command groups on multiple hosts and log results.")
    ap.add_argument("--hosts", required=True, help="comma-separated ssh hosts (e.g. root@1.2.3.4,root@1.2.3.5)")
    ap.add_argument("--plan", required=True, help="JSON file with list of command groups")
    ap.add_argument("--bin_root", default="",
                    help="path substituted for {BIN_ROOT} tokens in the plan "
                         "(typically the synchronization/ directory holding lock_bench)")
    ap.add_argument("--out_csv", default="synchronization/result/command_runs.csv", help="output CSV path")
    ap.add_argument("--ssh_opt", action="append", default=[], help="extra ssh option (repeatable)")
    ap.add_argument("--timeout", type=int, default=0, help="per-command timeout (seconds, 0=none)")
    ap.add_argument("--max_output", type=int, default=4000, help="truncate stdout/stderr to this length (0=none)")
    ap.add_argument("--non_init_delay", type=float, default=0.1, help="sleep for non-init hosts in stage mode (seconds)")
    ap.add_argument("--debug", action="store_true", help="print exact ssh command lines")
    ap.add_argument("--direct", action="store_true", help="run commands directly without bash -lc")
    ap.add_argument("--dry_run", action="store_true", help="print what would run and exit")

    args = ap.parse_args()

    hosts = [h.strip() for h in args.hosts.split(",") if h.strip()]
    if not hosts:
        print("no hosts specified", file=sys.stderr)
        return 1

    plan = load_plan(args.plan, args.bin_root)

    ssh_opts = DEFAULT_SSH_OPTS + args.ssh_opt
    timeout = args.timeout if args.timeout > 0 else None

    if args.dry_run:
        print("hosts:")
        for h in hosts:
            print(f"  {h}")
        print("plan:")
        for gi, group in enumerate(plan, 1):
            for ci, cmd in enumerate(group, 1):
                print(f"  group {gi} cmd {ci}: {cmd}")
        return 0

    header = [
        "timestamp_utc",
        "group",
        "cmd_index",
        "host",
        "command",
        "exit_code",
        "duration_s",
        "stdout",
        "stderr",
    ]
    ensure_csv_header(args.out_csv, header)

    for group_idx, group in enumerate(plan, 1):
        stage_hosts = None
        if len(group) <= len(hosts):
            counts = [extract_hosts_count(cmd) for cmd in group]
            if all(c is None for c in counts):
                stage_hosts = hosts[:len(group)]
            elif all(c == counts[0] for c in counts) and counts[0] == len(group):
                stage_hosts = hosts[:len(group)]
        if stage_hosts is not None:
            if len(stage_hosts) == len(hosts):
                print(f"[{now_utc_iso()}] group {group_idx} stage-per-host")
            else:
                print(f"[{now_utc_iso()}] group {group_idx} stage-per-host (first {len(stage_hosts)} hosts)")
            for idx, cmd in enumerate(group, 1):
                print(f"  host[{idx - 1}] cmd: {cmd}")
            results = run_command_per_host(stage_hosts, group, ssh_opts, timeout,
                                           args.non_init_delay, args.debug,
                                           args.direct)
            with open(args.out_csv, "a", newline="", encoding="utf-8") as f:
                writer = csv.writer(f)
                for res in results:
                    writer.writerow([
                        now_utc_iso(),
                        group_idx,
                        1,
                        res["host"],
                        res["command"],
                        res["exit_code"],
                        res["duration_s"],
                        truncate(res["stdout"], args.max_output),
                        truncate(res["stderr"], args.max_output),
                    ])
            continue

        for cmd_idx, cmd in enumerate(group, 1):
            print(f"[{now_utc_iso()}] group {group_idx} cmd {cmd_idx}: {cmd}")
            results = run_command_all(hosts, cmd, ssh_opts, timeout,
                                      args.debug, args.direct)
            with open(args.out_csv, "a", newline="", encoding="utf-8") as f:
                writer = csv.writer(f)
                for res in results:
                    writer.writerow([
                        now_utc_iso(),
                        group_idx,
                        cmd_idx,
                        res["host"],
                        res["command"],
                        res["exit_code"],
                        res["duration_s"],
                        truncate(res["stdout"], args.max_output),
                        truncate(res["stderr"], args.max_output),
                    ])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
