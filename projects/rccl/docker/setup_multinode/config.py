"""Configuration data structures for setup_multinode.

Defines the Action enum for mutually exclusive modes, SSHConfig for SSH-related
settings, and Config as the single source of truth for all runtime parameters.
All defaults can be overridden via environment variables or CLI flags.
"""

import os
from enum import Enum
from typing import List, Optional


class Action(Enum):
    """Mutually exclusive action modes."""
    BUILD = "build"
    RUN = "run"
    RUN_ONLY = "run-only"
    VERIFY = "verify"
    LAUNCH_ALL = "launch-all"
    STOP_ALL = "stop-all"
    SETUP_DEPS = "setup-deps"


class SSHConfig(object):
    """SSH-related settings with derived key path resolution."""

    def __init__(
        self,
        key=None,               # type: Optional[str]
        authorized_keys=None,   # type: Optional[str]
        keygen=False,           # type: bool
        key_dir=None,           # type: Optional[str]
        port=2224,              # type: int
    ):
        # type: (...) -> None
        self.key = key
        self.authorized_keys = authorized_keys
        self.keygen = keygen
        self.key_dir = key_dir or os.path.join(
            os.path.expanduser("~"), ".docker-ssh-keys"
        )
        self.port = port

    @property
    def priv_key(self):
        # type: () -> Optional[str]
        """Resolve private key path (handles *.pub input transparently)."""
        if not self.key:
            return None
        return self.key[:-4] if self.key.endswith(".pub") else self.key

    @property
    def pub_key(self):
        # type: () -> Optional[str]
        """Resolve public key path."""
        if not self.key:
            return None
        return self.key if self.key.endswith(".pub") else self.key + ".pub"


class Config(object):
    """All configuration for setup_multinode.

    Merges environment variables with defaults.  CLI flags are applied
    afterwards in ``__main__.py``.
    """

    def __init__(self):
        # type: () -> None
        home = os.path.expanduser("~")

        self.action = Action.BUILD
        self.rocm_image = os.environ.get(
            "ROCM_IMAGE", "rocm/dev-ubuntu-24.04:7.1.1-complete"
        )
        self.container_name = os.environ.get("CONTAINER_NAME", "rccl-mn")
        self.shm_size = os.environ.get("SHM_SIZE", "64g")
        self.gpus = os.environ.get("GPUS", "")
        self.gpus_explicit = False

        self.shared_dir = os.environ.get(
            "SHARED_DIR", os.path.join(home, ".docker-shared")
        )
        self.builds_dir = os.environ.get(
            "BUILDS_DIR", os.path.join(home, ".docker-builds")
        )
        self.hostfile = os.environ.get(
            "HOSTFILE", os.path.join(home, ".mpi_hostfile")
        )
        self.post_setup_dir = os.environ.get("POST_SETUP_DIR", "")
        self.host_ssh_port = int(os.environ.get("HOST_SSH_PORT", "22"))
        self.verbose = bool(os.environ.get("VERBOSE", ""))
        self.force_rebuild = False
        self.extra_volumes = []  # type: List[str]
        self.dockerfile = os.environ.get(
            "DOCKERFILE", "Dockerfile.Multinode.Ubuntu"
        )

        # Parallelism for multi-node operations (launch_all, stop_all, verify)
        self.parallel = 16

        # Container runtime selection ("docker" or future "pyxis")
        self.runtime_name = "docker"
        self.runtime = None  # type: Optional[object]  # Set by __main__ via get_runtime()

        self.ssh = SSHConfig(
            key=os.environ.get("SSH_KEY", "") or None,
            authorized_keys=os.environ.get("SSH_AUTHORIZED_KEYS", "") or None,
            key_dir=(
                os.environ.get("SSH_KEY_DIR", "")
                or os.path.join(home, ".docker-ssh-keys")
            ),
            port=int(os.environ.get("SSH_PORT", "2224")),
        )

        # Points to the docker/ directory (parent of the setup_multinode package)
        self.script_dir = os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))
        )

    @property
    def image_tag(self):
        # type: () -> str
        """Derive Docker image tag from the ROCm base image name."""
        if ":" in self.rocm_image:
            tag = self.rocm_image.rsplit(":", 1)[-1]
        else:
            tag = "latest"
        return "rocm-multinode:" + tag
