"""SSH key management and connectivity verification.

Handles:
  - Installing user-supplied or auto-generated SSH keys into the shared key dir
  - Writing the SSH client config for container-to-container access
  - Verifying passwordless SSH to every host in the hostfile (parallel)
"""

import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Dict, Tuple

from .config import Config
from .utils import log, log_verbose, Timer


def write_ssh_config(cfg):
    # type: (Config) -> None
    """Write SSH client config and set key/config permissions."""
    key_dir = cfg.ssh.key_dir
    config_path = os.path.join(key_dir, "config")

    with open(config_path, "w") as f:
        f.write("Host *\n")
        f.write("    StrictHostKeyChecking no\n")
        f.write("    UserKnownHostsFile /dev/null\n")
        f.write("    LogLevel ERROR\n")
        f.write("    Port {}\n".format(cfg.ssh.port))
        f.write("    IdentityFile ~/.ssh/id_rsa\n")

    os.chmod(os.path.join(key_dir, "id_rsa"), 0o600)
    for name in ("id_rsa.pub", "authorized_keys", "config"):
        path = os.path.join(key_dir, name)
        if os.path.isfile(path):
            os.chmod(path, 0o644)


def install_ssh_keys(cfg):
    # type: (Config) -> None
    """Install SSH keys into the shared key directory (idempotent)."""
    key_dir = cfg.ssh.key_dir
    id_rsa = os.path.join(key_dir, "id_rsa")

    if os.path.isfile(id_rsa):
        log("  SSH keys exist at {}".format(key_dir))
        return

    if cfg.ssh.key:
        _install_from_existing(cfg, key_dir)
    elif cfg.ssh.keygen:
        _generate_new_keys(cfg, key_dir, id_rsa)
    else:
        log("  No SSH keys configured (use --ssh-key or --ssh-keygen for multi-node)")
        log_verbose("Hint: for multi-node SSH, use one of:")
        log_verbose(
            "  --ssh-key ~/.ssh/id_rsa"
            "                                              "
            "# shared key pair"
        )
        log_verbose(
            "  --ssh-key ~/.ssh/id_rsa "
            "--ssh-authorized-keys ~/.ssh/authorized_keys "
            "# mesh SSH (per-node keys)"
        )
        log_verbose(
            "  --ssh-keygen"
            "                                                         "
            "# generate a new pair"
        )


def _install_from_existing(cfg, key_dir):
    # type: (Config, str) -> None
    priv = cfg.ssh.priv_key
    pub = cfg.ssh.pub_key
    log("  Configuring SSH keys from {}...".format(cfg.ssh.key))

    shutil.copy2(priv, os.path.join(key_dir, "id_rsa"))
    shutil.copy2(pub, os.path.join(key_dir, "id_rsa.pub"))

    ak_dst = os.path.join(key_dir, "authorized_keys")
    if cfg.ssh.authorized_keys:
        shutil.copy2(cfg.ssh.authorized_keys, ak_dst)
        with open(ak_dst, "a") as dst:
            with open(pub) as src:
                dst.write(src.read())
        log_verbose(
            "authorized_keys: merged from {} + {}".format(
                cfg.ssh.authorized_keys, pub
            )
        )
    else:
        shutil.copy2(pub, ak_dst)

    write_ssh_config(cfg)
    log("  SSH keys configured at {}".format(key_dir))


def _generate_new_keys(cfg, key_dir, id_rsa):
    # type: (Config, str, str) -> None
    log("  Generating shared SSH keys...")
    subprocess.run(
        [
            "ssh-keygen", "-t", "rsa", "-b", "4096",
            "-N", "", "-f", id_rsa, "-C", "docker-shared-key", "-q",
        ],
        check=True,
    )

    pub_file = id_rsa + ".pub"
    ak_dst = os.path.join(key_dir, "authorized_keys")

    if cfg.ssh.authorized_keys:
        shutil.copy2(cfg.ssh.authorized_keys, ak_dst)
        with open(ak_dst, "a") as dst:
            with open(pub_file) as src:
                dst.write(src.read())
        log_verbose(
            "authorized_keys: merged from {} + generated key".format(
                cfg.ssh.authorized_keys
            )
        )
    else:
        shutil.copy2(pub_file, ak_dst)

    write_ssh_config(cfg)
    log("  Keys generated at {}".format(key_dir))


# ---------------------------------------------------------------------------
# SSH connectivity verification
# ---------------------------------------------------------------------------
def verify_ssh(cfg):
    # type: (Config) -> None
    """Verify SSH connectivity to all hosts in the hostfile."""
    from .utils import parse_hostfile

    with Timer("SSH verification"):
        log("=== Verifying SSH connectivity (port {}) ===".format(cfg.ssh.port))

        if not os.path.isfile(cfg.hostfile):
            log("  Hostfile not found: {}".format(cfg.hostfile))
            log(
                "  Create it first:  echo 'hostname slots=8' > {}".format(
                    cfg.hostfile
                )
            )
            sys.exit(1)

        log_verbose("Hostfile: {}".format(cfg.hostfile))
        if cfg.verbose:
            log_verbose("Hostfile contents:")
            with open(cfg.hostfile) as f:
                for line in f:
                    log_verbose("  {}".format(line.rstrip()))

        hosts = parse_hostfile(cfg.hostfile)

        ssh_key = os.path.join(cfg.ssh.key_dir, "id_rsa")
        if not os.path.isfile(ssh_key):
            log("  Shared SSH key not found: {}".format(ssh_key))
            log("")
            log("  Set up SSH keys first:")
            log(
                "    python3 -m setup_multinode --launch-all "
                "--ssh-key ~/.ssh/id_rsa   # use your key pair"
            )
            log(
                "    python3 -m setup_multinode --launch-all "
                "--ssh-keygen              # generate a new pair"
            )
            log(
                "  For mesh SSH (per-node keys), also pass "
                "--ssh-authorized-keys"
            )
            sys.exit(1)

        log_verbose("Using SSH key: {}".format(ssh_key))

        ssh_opts = [
            "-p", str(cfg.ssh.port),
            "-i", ssh_key,
            "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=/dev/null",
            "-o", "ConnectTimeout=5",
            "-o", "BatchMode=yes",
            "-o", "LogLevel=ERROR",
        ]

        test_users = ["root", "ubuntu"]
        max_workers = min(cfg.parallel, len(hosts))

        log_verbose(
            "Verifying {} hosts x {} users ({} parallel)".format(
                len(hosts), len(test_users), max_workers,
            )
        )

        def _check_one(host, user):
            # type: (str, str) -> Tuple[str, str, bool]
            result = subprocess.run(
                ["ssh"] + ssh_opts
                + ["{}@{}".format(user, host), "hostname"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            return host, user, result.returncode == 0

        # Run checks in parallel
        results = {}  # type: Dict[Tuple[str, str], bool]
        with ThreadPoolExecutor(max_workers=max_workers) as pool:
            futures = {}
            for host in hosts:
                for user in test_users:
                    f = pool.submit(_check_one, host, user)
                    futures[f] = (host, user)
            for future in as_completed(futures):
                host, user = futures[future]
                try:
                    _, _, ok = future.result()
                    results[(host, user)] = ok
                except Exception:
                    results[(host, user)] = False

        # Print in hostfile order
        failed = False
        for host in hosts:
            for user in test_users:
                ok = results[(host, user)]
                if ok:
                    log("  [OK]   {}@{}".format(user, host))
                else:
                    log("  [FAIL] {}@{}".format(user, host))
                    failed = True
                    if cfg.verbose:
                        _log_ssh_debug(ssh_opts, user, host)

        if failed:
            _print_ssh_fix_hints(cfg)
            sys.exit(1)

    log("")
    log(
        "All hosts reachable (as {}). Ready for MPI workloads.".format(
            " ".join(test_users)
        )
    )


def _log_ssh_debug(ssh_opts, user, host):
    # type: (list, str, str) -> None
    log_verbose("SSH debug for {}@{}:".format(user, host))
    debug_result = subprocess.run(
        ["ssh", "-v"] + ssh_opts + ["{}@{}".format(user, host), "hostname"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    stderr = debug_result.stderr.decode("utf-8", errors="replace")
    for line in stderr.splitlines()[-20:]:
        log_verbose("  {}".format(line))


def _print_ssh_fix_hints(cfg):
    # type: (Config) -> None
    log("")
    log("Fix failed hosts:")
    log("  1. Ensure the container is running: docker ps")
    log(
        "  2. Check sshd: docker exec <container> "
        "ss -tlnp | grep {}".format(cfg.ssh.port)
    )
    log(
        "  3. Restart sshd: docker exec <container> "
        "/usr/sbin/sshd -p{}".format(cfg.ssh.port)
    )
    log("")
    log("  If SSH keys are not set up, re-launch with:")
    log(
        "    python3 -m setup_multinode --launch-all "
        "--ssh-key ~/.ssh/id_rsa   # use your key pair"
    )
    log(
        "    python3 -m setup_multinode --launch-all "
        "--ssh-keygen              # generate a new pair"
    )
    log("  For mesh SSH (per-node keys), also pass --ssh-authorized-keys")
