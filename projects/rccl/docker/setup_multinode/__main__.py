"""CLI entry point for setup_multinode.

Invoke as:
    python3 -m setup_multinode [OPTIONS] [ROCM_IMAGE]
    python3 run_multinode.py [OPTIONS] [ROCM_IMAGE]
"""

import argparse
import subprocess
import sys

from .config import Action, Config
from .utils import error, log, log_verbose, set_verbose, expand_path, auto_detect_gpus
from .validate import validate
from .slurm import detect_slurm
from .runtime import get_runtime
from .ssh import verify_ssh
from .deps import setup_shared_deps
from .orchestrate import setup_host, launch_all, stop_all


def build_parser():
    # type: () -> argparse.ArgumentParser
    """Construct the argument parser with all flags."""
    parser = argparse.ArgumentParser(
        prog="setup_multinode",
        description="Build and launch multi-node ROCm Docker containers.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  python3 -m setup_multinode                                          # build default
  python3 -m setup_multinode rocm/dev-ubuntu-24.04:7.1.1-complete     # specific ROCm
  python3 -m setup_multinode --run --gpus 16                          # build + launch
  python3 -m setup_multinode --run --volume /data:/data --name node0  # extra mount
  python3 -m setup_multinode --run-only rocm-multinode:7.1.1-complete # launch existing
  python3 -m setup_multinode --verify                                 # check SSH
  python3 -m setup_multinode --launch-all --ssh-keygen                # build+launch, auto SSH
  python3 -m setup_multinode --launch-all --ssh-key ~/.ssh/id_rsa     # use your keys
  python3 -m setup_multinode --launch-all --ssh-key ~/.ssh/id_rsa \\
                             --ssh-authorized-keys ~/.ssh/authorized_keys  # mesh SSH
  python3 -m setup_multinode --setup-deps                             # build shared UCX/MPI
  python3 -m setup_multinode --stop-all                               # stop everywhere
  python3 -m setup_multinode --launch-all --parallel 64               # max parallelism

Environment Variables (alternative to flags):
  ROCM_IMAGE, CONTAINER_NAME, SHARED_DIR, BUILDS_DIR, SSH_KEY_DIR,
  SSH_KEY, SSH_AUTHORIZED_KEYS, HOSTFILE, SSH_PORT, GPUS,
  POST_SETUP_DIR, HOST_SSH_PORT, DOCKERFILE, VERBOSE

Path expansion:
  All path options support ~ and $VAR / ${VAR} expansion.
""",
    )

    parser.add_argument(
        "rocm_image", nargs="?", default=None,
        help=(
            "ROCm base image "
            "(default: ROCM_IMAGE env or rocm/dev-ubuntu-24.04:7.1.1-complete)"
        ),
    )

    # --- Mutually exclusive action modes ---
    action = parser.add_mutually_exclusive_group()
    action.add_argument(
        "--run", dest="action", action="store_const", const=Action.RUN,
        help="Build image and launch a container",
    )
    action.add_argument(
        "--run-only", dest="action", action="store_const",
        const=Action.RUN_ONLY,
        help="Launch a container (skip build, image must exist)",
    )
    action.add_argument(
        "--verify", dest="action", action="store_const",
        const=Action.VERIFY,
        help="Verify SSH connectivity to all hosts in the hostfile",
    )
    action.add_argument(
        "--launch-all", dest="action", action="store_const",
        const=Action.LAUNCH_ALL,
        help="Build + launch on ALL nodes in the hostfile via SSH",
    )
    action.add_argument(
        "--stop-all", dest="action", action="store_const",
        const=Action.STOP_ALL,
        help="Stop + remove containers on ALL nodes in the hostfile",
    )
    action.add_argument(
        "--setup-deps", dest="action", action="store_const",
        const=Action.SETUP_DEPS,
        help="Build shared deps (UCX, OpenMPI) into shared dir (once)",
    )

    # --- Container settings ---
    parser.add_argument(
        "--name", dest="container_name",
        help="Container name (default: rccl-mn)",
    )
    parser.add_argument(
        "--gpus", type=int,
        help="Number of GPUs (default: auto-detect)",
    )
    parser.add_argument(
        "--ssh-port", type=int,
        help="SSH port inside container (default: 2224)",
    )
    parser.add_argument(
        "--shm-size",
        help="Shared memory size (default: 64g)",
    )

    # --- Path options ---
    parser.add_argument(
        "--hostfile",
        help="MPI hostfile on the host (default: ~/.mpi_hostfile)",
    )
    parser.add_argument(
        "--shared-dir",
        help="Shared workspace on host (default: ~/.docker-shared)",
    )
    parser.add_argument(
        "--builds-dir",
        help="Shared builds dir on host (default: ~/.docker-builds)",
    )
    parser.add_argument(
        "--ssh-key-dir",
        help="SSH key directory on host (default: ~/.docker-ssh-keys)",
    )
    parser.add_argument(
        "--volume", "-v", dest="volumes", action="append", default=[],
        help="Extra host volume mount SRC:DST (repeatable)",
    )
    parser.add_argument(
        "--post-setup", dest="post_setup_dir",
        help="Post-setup dir with setup.sh/env.sh (optional)",
    )

    # --- SSH key options ---
    parser.add_argument(
        "--ssh-key",
        help="Use existing SSH key pair for inter-container SSH",
    )
    parser.add_argument(
        "--ssh-authorized-keys",
        help="Custom authorized_keys file (for mesh SSH setups)",
    )
    parser.add_argument(
        "--ssh-keygen", action="store_true",
        help="Auto-generate a shared SSH key pair",
    )

    # --- Modifiers ---
    parser.add_argument(
        "--dockerfile",
        help=(
            "Dockerfile to use for image build "
            "(default: DOCKERFILE env or Dockerfile.Multinode.Ubuntu)"
        ),
    )
    parser.add_argument(
        "--rebuild", action="store_true",
        help="Force image rebuild and replace existing containers",
    )
    parser.add_argument(
        "--host-ssh-port", type=int,
        help="SSH port for host-to-host access (default: 22)",
    )
    parser.add_argument(
        "--parallel", type=int,
        help=(
            "Max nodes to operate on concurrently for "
            "--launch-all, --stop-all, --verify (default: 16)"
        ),
    )
    parser.add_argument(
        "--runtime", dest="runtime_name",
        choices=["docker"],
        help="Container runtime (default: docker)",
    )
    parser.add_argument(
        "--verbose", action="store_true",
        help="Enable detailed debug logging",
    )

    return parser


def _apply_cli_args(cfg, args):
    # type: (Config, argparse.Namespace) -> None
    """Overlay CLI arguments onto the Config (only if explicitly provided)."""
    if args.action is not None:
        cfg.action = args.action
    if args.rocm_image is not None:
        cfg.rocm_image = args.rocm_image
    if args.container_name is not None:
        cfg.container_name = args.container_name
    if args.gpus is not None:
        cfg.gpus = str(args.gpus)
        cfg.gpus_explicit = True
    if args.ssh_port is not None:
        cfg.ssh.port = args.ssh_port
    if args.shm_size is not None:
        cfg.shm_size = args.shm_size
    if args.hostfile is not None:
        cfg.hostfile = args.hostfile
    if args.shared_dir is not None:
        cfg.shared_dir = args.shared_dir
    if args.builds_dir is not None:
        cfg.builds_dir = args.builds_dir
    if args.ssh_key_dir is not None:
        cfg.ssh.key_dir = args.ssh_key_dir
    if args.volumes:
        cfg.extra_volumes = list(args.volumes)
    if args.post_setup_dir is not None:
        cfg.post_setup_dir = args.post_setup_dir
    if args.ssh_key is not None:
        cfg.ssh.key = args.ssh_key
    if args.ssh_authorized_keys is not None:
        cfg.ssh.authorized_keys = args.ssh_authorized_keys
    if args.ssh_keygen:
        cfg.ssh.keygen = True
    if args.dockerfile is not None:
        cfg.dockerfile = args.dockerfile
    if args.rebuild:
        cfg.force_rebuild = True
    if args.host_ssh_port is not None:
        cfg.host_ssh_port = args.host_ssh_port
    if args.parallel is not None:
        cfg.parallel = args.parallel
    if args.runtime_name is not None:
        cfg.runtime_name = args.runtime_name
    if args.verbose:
        cfg.verbose = True


def _expand_paths(cfg):
    # type: (Config) -> None
    """Expand ~ and $VAR in all path-valued config fields."""
    cfg.shared_dir = expand_path(cfg.shared_dir)
    cfg.builds_dir = expand_path(cfg.builds_dir)
    cfg.ssh.key_dir = expand_path(cfg.ssh.key_dir)
    cfg.hostfile = expand_path(cfg.hostfile)
    if cfg.post_setup_dir:
        cfg.post_setup_dir = expand_path(cfg.post_setup_dir)
    if cfg.ssh.key:
        cfg.ssh.key = expand_path(cfg.ssh.key)
    if cfg.ssh.authorized_keys:
        cfg.ssh.authorized_keys = expand_path(cfg.ssh.authorized_keys)
    cfg.extra_volumes = [expand_path(v) for v in cfg.extra_volumes]


def _dump_config(cfg):
    # type: (Config) -> None
    """Print all configuration values in verbose mode."""
    log("=== Verbose mode enabled ===")
    log_verbose("script_dir={}".format(cfg.script_dir))
    log_verbose("rocm_image={}".format(cfg.rocm_image))
    log_verbose("image_tag={}".format(cfg.image_tag))
    log_verbose("container_name={}".format(cfg.container_name))
    log_verbose("gpus={}".format(cfg.gpus))
    log_verbose("ssh.port={}".format(cfg.ssh.port))
    log_verbose("shm_size={}".format(cfg.shm_size))
    log_verbose("shared_dir={}".format(cfg.shared_dir))
    log_verbose("builds_dir={}".format(cfg.builds_dir))
    log_verbose("ssh.key_dir={}".format(cfg.ssh.key_dir))
    log_verbose("hostfile={}".format(cfg.hostfile))
    log_verbose("post_setup_dir={}".format(cfg.post_setup_dir))
    log_verbose("ssh.key={}".format(cfg.ssh.key or ""))
    log_verbose("ssh.authorized_keys={}".format(
        cfg.ssh.authorized_keys or ""
    ))
    log_verbose("ssh.keygen={}".format(cfg.ssh.keygen))
    log_verbose("action={}".format(cfg.action.value))
    log_verbose("host_ssh_port={}".format(cfg.host_ssh_port))
    log_verbose("dockerfile={}".format(cfg.dockerfile))
    log_verbose("force_rebuild={}".format(cfg.force_rebuild))
    log_verbose("parallel={}".format(cfg.parallel))
    log_verbose("runtime={}".format(cfg.runtime_name))
    log_verbose("extra_volumes={}".format(cfg.extra_volumes))
    import platform
    log_verbose("Host kernel: {}".format(platform.release()))
    log("")


def _run():
    # type: () -> None
    """Core logic: parse args, configure, validate, dispatch."""
    parser = build_parser()
    args = parser.parse_args()

    # --- Build configuration ---
    cfg = Config()
    _apply_cli_args(cfg, args)
    _expand_paths(cfg)

    # Auto-detect GPUs if not set
    if not cfg.gpus:
        cfg.gpus = str(auto_detect_gpus())

    # Enable verbose logging
    set_verbose(cfg.verbose)

    # Instantiate the container runtime
    cfg.runtime = get_runtime(cfg)

    # SLURM auto-detection (may mutate hostfile and ssh settings)
    detect_slurm(cfg)

    # Validate configuration
    validate(cfg)

    # Verbose config dump
    if cfg.verbose:
        _dump_config(cfg)

    # --- Dispatch ---
    if cfg.action == Action.VERIFY:
        verify_ssh(cfg)

    elif cfg.action == Action.STOP_ALL:
        stop_all(cfg)

    elif cfg.action == Action.SETUP_DEPS:
        setup_host(cfg)
        cfg.runtime.build_image()
        setup_shared_deps(cfg)

    elif cfg.action == Action.LAUNCH_ALL:
        setup_host(cfg)
        cfg.runtime.build_image()
        setup_shared_deps(cfg)
        launch_all(cfg)

    elif cfg.action == Action.RUN_ONLY:
        setup_host(cfg)
        cfg.runtime.launch()

    elif cfg.action == Action.RUN:
        setup_host(cfg)
        cfg.runtime.build_image()
        cfg.runtime.launch()

    else:
        # Default: build only
        setup_host(cfg)
        cfg.runtime.build_image()
        log("Image ready: {}".format(cfg.image_tag))
        log("")
        log("To launch a container:")
        log(
            "  python3 -m setup_multinode --run [--name NAME] [--gpus N]"
        )
        log("")


def main():
    # type: () -> None
    """Entry point with top-level exception handling for partial failures."""
    try:
        _run()
    except KeyboardInterrupt:
        log("")
        log("Interrupted.")
        log("  Partial setup may exist. To clean up:")
        log("    python3 -m setup_multinode --stop-all")
        sys.exit(130)
    except SystemExit:
        raise
    except subprocess.CalledProcessError as e:
        log("")
        error("Command failed (exit {}): {}".format(
            e.returncode,
            " ".join(str(a) for a in e.cmd) if e.cmd else "(unknown)",
        ))
        log("  Partial setup may exist. To clean up:")
        log("    python3 -m setup_multinode --stop-all")
        sys.exit(1)
    except Exception as e:
        log("")
        error("Unexpected error: {}".format(e))
        log("  Partial setup may exist. To clean up:")
        log("    python3 -m setup_multinode --stop-all")
        sys.exit(1)


if __name__ == "__main__":
    main()
