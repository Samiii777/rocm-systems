"""Multi-node orchestration: host setup, launch-all, stop-all.

The host-setup phase creates shared directories and installs SSH keys.
launch_all / stop_all fan out to every node in the hostfile by spawning
all SSH commands as concurrent subprocesses (Popen), then polling for
completion.  No threads are used — the OS handles parallelism.
"""

import os
import subprocess
import sys
import time
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
# Helpers
# ---------------------------------------------------------------------------
def _ssh_base_cmd(cfg, host):
    # type: (Config, str) -> List[str]
    """Build the SSH prefix for reaching *host*."""
    return [
        "ssh",
        "-p", str(cfg.host_ssh_port),
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "ConnectTimeout=10",
        "-o", "BatchMode=yes",
        "-o", "LogLevel=ERROR",
        host,
    ]


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
    if cfg.ssh.keygen:
        args.append("--ssh-keygen")
    for vol in cfg.extra_volumes:
        args += ["--volume", vol]
    args.append(cfg.rocm_image)
    return args


# ---------------------------------------------------------------------------
# Launch containers on all nodes (parallel via Popen)
# ---------------------------------------------------------------------------
def launch_all(cfg):
    # type: (Config) -> None
    """Build + launch a container on every node in the hostfile.

    All SSH commands are spawned concurrently as subprocesses, then
    polled for completion so progress is printed in real time.
    """
    with Timer("Launch all nodes"):
        log("=== Launching containers on all nodes ===")
        log("")

        hosts = parse_hostfile(cfg.hostfile)
        local_names = get_local_hostnames()
        script = os.path.join(cfg.script_dir, "run_multinode.py")

        log("  Hostfile  : {} ({} nodes)".format(cfg.hostfile, len(hosts)))
        log("  Image     : {}".format(cfg.image_tag))
        log("  Container : {}".format(cfg.container_name))
        log_verbose("Script    : {}".format(script))
        log("")

        forward_args = _build_forward_args(cfg)
        node_cmd = ["python3", script] + forward_args

        # Spawn all nodes at once
        procs = {}  # type: Dict[str, subprocess.Popen]
        for host in hosts:
            if host in local_names:
                procs[host] = subprocess.Popen(
                    node_cmd,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                )
            else:
                procs[host] = subprocess.Popen(
                    _ssh_base_cmd(cfg, host) + node_cmd,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                )

        # Poll until all complete, printing progress as each finishes
        results = {}  # type: Dict[str, Tuple[int, bytes]]
        done = 0
        while done < len(procs):
            for host, proc in procs.items():
                if host in results:
                    continue
                rc = proc.poll()
                if rc is not None:
                    output = proc.stdout.read() if proc.stdout else b""
                    results[host] = (rc, output)
                    done += 1
                    status = "[OK]  " if rc == 0 else "[FAIL]"
                    log("  {} {:<40} ({}/{})".format(
                        status, host, done, len(hosts),
                    ))
            if done < len(procs):
                time.sleep(0.5)

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
# Stop containers on all nodes (parallel via Popen)
# ---------------------------------------------------------------------------
def stop_all(cfg):
    # type: (Config) -> None
    """Stop and remove containers on every node in the hostfile."""
    log("=== Stopping containers on all nodes ===")
    log("")

    hosts = parse_hostfile(cfg.hostfile)
    local_names = get_local_hostnames()
    stop_cmd = cfg.runtime.get_stop_cmd()

    # Spawn all stop commands at once
    procs = {}  # type: Dict[str, subprocess.Popen]
    for host in hosts:
        if host in local_names:
            procs[host] = subprocess.Popen(
                stop_cmd, shell=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
        else:
            procs[host] = subprocess.Popen(
                _ssh_base_cmd(cfg, host) + [stop_cmd],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )

    # Collect results (all procs are already running concurrently)
    unreachable = []
    for host in hosts:
        proc = procs[host]
        stdout, stderr = proc.communicate()
        output = stdout.decode("utf-8", errors="replace").strip() if stdout else ""
        if proc.returncode != 0:
            err = stderr.decode("utf-8", errors="replace").strip() if stderr else ""
            output = "[UNREACHABLE] {}".format(
                err[:200] if err else "exit {}".format(proc.returncode)
            )
            unreachable.append(host)
        log("  {:<20} {}".format(host, output))

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
