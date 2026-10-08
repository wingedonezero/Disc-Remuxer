"""libdvdnav's VM: the Rnd source the self-test hooks call."""

from helpers import extern

RND = {}
"""RND["fn"]: the Python function the VM's Rnd operation takes its numbers from."""


@extern
def tb_py_rnd(priv):
    return RND["fn"]()
