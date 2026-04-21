"""Multi-node orchestration: host setup, launch-all, stop-all.

The host-setup phase creates shared directories and installs SSH keys.
launch_all / stop_all fan out to every node in the hostfile, using
``concurrent.futures.ThreadPoolExecutor`` for parallel execution so
that the framework scales to 64+ nodes without linear slowdown.
"""

import os
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Dict, List, Tuple

from .config import Config
from .ssh import install_ssh_keys
from .utils import (
    log, log_verbose, parse_hostfile, get_local_hostnames, Timer,
)


# ---------------------------------------------------------------------------
# Host setup (shared dirs + SSH keys) — idempotent
# ---------------------------------------------------------------------------
def setup_host(cfg):
    # type: (Config) -> None
    """Create shared directories and install SSH keys."""
    with Timer("Host setup"):
        log("=== Host setup ===")

        for d in (cfg.shared_dir, cfg.builds_dir):
            if not os.path.isdir(d):
                os.makedirs(d, exist_ok=True)
                try:
                    os.chmod(d, 0o777)
                except OSError:
                    try:
                        os.chmod(d, 0o755)
                    except OSError:
                        pass
                log_verbose("Created {}".format(d))
            else:
                log_verbose("Exists  {}".format(d))

        key_dir = cfg.ssh.key_dir
        if not os.path.isdir(key_dir):
            os.makedirs(key_dir, exist_ok=True)
            os.chmod(key_dir, 0o700)
            log_verbose("Created {} (mode 700)".format(key_dir))
        else:
            log_verbose("Exists  {}".format(key_dir))

        install_ssh_keys(cfg)

        if cfg.verbose:
            log_verbose("SSH key dir contents:")
            if os.path.isdir(key_dir):
                for entry in sorted(os.listdir(key_dir)):
                    log_verbose("  {}".format(entry))
            hf = cfg.hostfile
            if os.path.isfile(hf):
                with open(hf) as f:
                    n = sum(1 for _ in f)
                log_verbose(
                    "Hostfile: {} (exists, {} lines)".format(hf, n)
                )
            else:
                log_verbose("Hostfile: {} (not found)".format(hf))

    log("")


# ---------------------------------------------------------------------------
# SSH wrapper for host-to-host access (port 22 by default)
# ---------------------------------------------------------------------------
def _host_ssh(cfg, host, cmd, capture=False):
    # type: (Config, str, ..., bool) -> subprocess.CompletedProcess
    """Run a command on a remote host via SSH."""
    ssh_base = [
        "ssh",
        "-p", str(cfg.host_ssh_port),
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "ConnectTimeout=10",
        "-o", "BatchMode=yes",
        "-o", "LogLevel=ERROR",
        host,
    ]
    if isinstance(cmd, str):
        ssh_cmd = ssh_base + [cmd]
    else:
        ssh_cmd = ssh_base + list(cmd)

    kwargs = {}
    if capture:
        kwargs["stdout"] = subprocess.PIPE
        kwargs["stderr"] = subprocess.PIPE
    return subprocess.run(ssh_cmd, **kwargs)


# ---------------------------------------------------------------------------
# Forward-argument assembly for remote invocations
# ---------------------------------------------------------------------------
def _build_forward_args(cfg):
    # type: (Config) -> List[str]
    """Build CLI args to forward when invoking per-node setup."""
    args = [
        "--run",
        "--name", cfg.container_name,
        "--ssh-port", str(cfg.ssh.port),
        "--shm-size", cfg.shm_size,
        "--shared-dir", cfg.shared_dir,
        "--builds-dir", cfg.builds_dir,
        "--ssh-key-dir", cfg.ssh.key_dir,
        "--hostfile", cfg.hostfile,
        "--runtime", cfg.runtime_name,
    ]
    if cfg.gpus_explicit:
        args += ["--gpus", cfg.gpus]
    if cfg.dockerfile != "Dockerfile.Multinode.Ubuntu":
        args += ["--dockerfile", cfg.dockerfile]
    if cfg.force_rebuild:
        args.append("--rebuild")
    if cfg.verbose:
        args.append("--verbose")
    if cfg.post_setup_dir:
        args += ["--post-setup", cfg.post_setup_dir]
    if cfg.ssh.key:
        args += ["--ssh-key", cfg.ssh.key]
    if cfg.ssh.authorized_keys:
        args += ["--ssh-authorized-keys", cfg.ssh.authorized_keys]
    if cfg.ssh.keygen:
        args.append("--ssh-keygen")
    for vol in cfg.extra_volumes:
        args += ["--volume", vol]
    args.append(cfg.rocm_image)
    return args


# ---------------------------------------------------------------------------
# Launch containers on all nodes (parallel)
# ---------------------------------------------------------------------------
def launch_all(cfg):
    # type: (Config) -> None
    """Build + launch a container on every node in the hostfile.

    Uses ThreadPoolExecutor to run up to ``cfg.parallel`` nodes
    concurrently.  Per-node output is captured and replayed only on
    failure so that parallel execution does not produce interleaved logs.
    """
    with Timer("Launch all nodes"):
        log("=== Launching containers on all nodes ===")
        log("")

        hosts = parse_hostfile(cfg.hostfile)
        local_names = get_local_hostnames()
        script = os.path.join(cfg.script_dir, "run_multinode.py")
        max_workers = min(cfg.parallel, len(hosts))

        log("  Hostfile  : {} ({} nodes)".format(cfg.hostfile, len(hosts)))
        log("  Image     : {}".format(cfg.image_tag))
        log("  Container : {}".format(cfg.container_name))
        log("  Parallel  : {}".format(max_workers))
        log_verbose("Script    : {}".format(script))
        log("")

        forward_args = _build_forward_args(cfg)

        # Thread-safe progress counter
        _lock = threading.Lock()
        _done = [0]

        def _launch_one(host):
            # type: (str) -> Tuple[str, int, bytes]
            cmd = ["python3", script] + forward_args
            if host in local_names:
                result = subprocess.run(
                    cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                )
            else:
                ssh_base = [
                    "ssh",
                    "-p", str(cfg.host_ssh_port),
                    "-o", "StrictHostKeyChecking=no",
                    "-o", "UserKnownHostsFile=/dev/null",
                    "-o", "ConnectTimeout=10",
                    "-o", "BatchMode=yes",
                    "-o", "LogLevel=ERROR",
                    host,
                ] + cmd
                result = subprocess.run(
                    ssh_base,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                )
            with _lock:
                _done[0] += 1
                status = "[OK]  " if result.returncode == 0 else "[FAIL]"
                log("  {} {:<40} ({}/{})".format(
                    status, host, _done[0], len(hosts),
                ))
            return host, result.returncode, result.stdout or b""

        # Execute in parallel
        results = {}  # type: Dict[str, Tuple[int, bytes]]
        with ThreadPoolExecutor(max_workers=max_workers) as pool:
            futures = {
                pool.submit(_launch_one, h): h for h in hosts
            }
            for future in as_completed(futures):
                host = futures[future]
                try:
                    host_name, rc, output = future.result()
                    results[host_name] = (rc, output)
                except Exception as exc:
                    with _lock:
                        _done[0] += 1
                        log("  [FAIL] {:<40} ({}/{}) {}".format(
                            host, _done[0], len(hosts),
                            type(exc).__name__,
                        ))
                    results[host] = (
                        1,
                        str(exc).encode("utf-8", errors="replace"),
                    )

        failed = [h for h in hosts if results.get(h, (1, b""))[0] != 0]
        succeeded = [h for h in hosts if results.get(h, (1, b""))[0] == 0]

        if failed:
            _report_launch_failures(cfg, hosts, results, failed, succeeded)
            sys.exit(1)

    log("")
    log("=== All {} containers launched ===".format(len(hosts)))
    log("")
    log("  Verify container SSH:")
    log("    python3 -m setup_multinode --verify")


# ---------------------------------------------------------------------------
# Partial-failure reporting for launch_all
# ---------------------------------------------------------------------------
def _report_launch_failures(cfg, hosts, results, failed, succeeded):
    # type: (Config, List[str], Dict[str, Tuple[int, bytes]], List[str], List[str]) -> None
    """Print detailed failure info, partial-deployment summary, and next steps."""
    log("")
    log("=== Failed nodes ({}/{}) ===".format(len(failed), len(hosts)))
    for host in failed:
        rc, output = results[host]
        log("")
        log("--- {} (exit {}) ---".format(host, rc))
        text = output.decode("utf-8", errors="replace")
        for line in text.splitlines()[-30:]:
            log("  {}".format(line))

    log("")
    log("=== Partial deployment summary ===")
    log("  Succeeded : {}/{} nodes".format(len(succeeded), len(hosts)))
    log("  Failed    : {}/{} nodes".format(len(failed), len(hosts)))
    if len(failed) <= 20:
        log("  Failed on : {}".format(", ".join(failed)))
    log("")
    log("  Next steps:")
    log("    1. Retry (idempotent — skips already-running containers):")
    log("       python3 -m setup_multinode --launch-all")
    log("    2. Clean up ALL nodes and start fresh:")
    log("       python3 -m setup_multinode --stop-all")


# ---------------------------------------------------------------------------
# Stop containers on all nodes (parallel)
# ---------------------------------------------------------------------------
def stop_all(cfg):
    # type: (Config) -> None
    """Stop and remove containers on every node in the hostfile."""
    log("=== Stopping containers on all nodes ===")
    log("")

    hosts = parse_hostfile(cfg.hostfile)
    local_names = get_local_hostnames()
    stop_cmd = cfg.runtime.get_stop_cmd()
    max_workers = min(cfg.parallel, len(hosts))

    def _stop_one(host):
        # type: (str) -> Tuple[str, int, str]
        if host in local_names:
            result = subprocess.run(
                stop_cmd, shell=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
        else:
            result = _host_ssh(cfg, host, stop_cmd, capture=True)
        output = ""
        if result.stdout:
            output = result.stdout.decode(
                "utf-8", errors="replace"
            ).strip()
        if result.returncode != 0:
            stderr = ""
            if result.stderr:
                stderr = result.stderr.decode(
                    "utf-8", errors="replace"
                ).strip()
            output = "[UNREACHABLE] {}".format(
                stderr[:200] if stderr else "exit {}".format(
                    result.returncode
                )
            )
        return host, result.returncode, output

    results = {}  # type: Dict[str, Tuple[int, str]]
    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = {pool.submit(_stop_one, h): h for h in hosts}
        for future in as_completed(futures):
            host = futures[future]
            try:
                host_name, rc, output = future.result()
                results[host_name] = (rc, output)
            except Exception as exc:
                results[host] = (1, "[ERROR] {}".format(exc))

    # Print in hostfile order
    unreachable = []
    for host in hosts:
        rc, output = results.get(host, (1, "[ERROR] no result"))
        log("  {:<20} {}".format(host, output))
        if rc != 0:
            unreachable.append(host)

    if unreachable:
        log("")
        log("WARNING: {} node(s) could not be reached for cleanup:".format(
            len(unreachable)
        ))
        for h in unreachable:
            log("  - {}".format(h))
        log("")
        log("  Manual cleanup on unreachable nodes:")
        log("    ssh <node> docker rm -f {}".format(cfg.container_name))

    log("")
    log("=== Done ===")
