# -*- coding: utf-8 -*-
r"""
This package is lazily initialized, so you can always import it.
"""
import sys
import types
from typing import List, Optional, Tuple, Union, Dict

import torch
import intel_extension_for_pytorch

from torch import serialization
from torch.storage import _StorageBase, _LegacyStorage, _warn_typed_storage_removal
from torch._utils import classproperty
from torch.xpu._utils import _get_device_index
from torch.xpu import _lazy_init, _lazy_call
from .intrinsic import *
from .cpp_extension import *
from .amp import *
from .utils import *
from .deterministics import *
from .memory import *
from ..utils.channels_last_1d import is_contiguous_channels_last_1d, to_channels_last_1d
from ..utils.utils import has_xpu
from .deepspeed import *

from .overrides import (
    override_tensor_totype,
    override_assert_equal,
)

from .graphs import (
    XPUGraph,
    graph,
    graph_pool_handle,
    is_current_stream_capturing,
    make_graphed_callables,
)

import intel_extension_for_pytorch.optim as optim
from intel_extension_for_pytorch._version import (
    __version__,
    __ipex_gitrev__,
    __torch_gitrev__,
    __gpu_onednn_gitrev__,
    __build_type__,
)


def init():
    intel_extension_for_pytorch._C._initExtension()


def is_initialized() -> bool:
    # for sphinx build
    return True


def _xpu_tag(obj):
    if obj.device.type == "xpu":
        return "xpu:" + str(obj.device.index)


def validate_xpu_device(location):
    device = _get_device_index(location, True)
    if not torch.xpu.is_available():
        raise RuntimeError(
            "Attempting to deserialize object on a xpu "
            "device but torch.xpu.is_available() is False. "
            "If you are running on a CPU-only machine, "
            "please use torch.load with map_location=torch.device('cpu') "
            "to map your storages to the CPU."
        )
    device_count = torch.xpu.device_count()
    if device >= device_count:
        raise RuntimeError(
            "Attempting to deserialize object on xpu device "
            f"{device} but torch.xpu.device_count() is {device_count}. Please use "
            "torch.load with map_location to map your storages "
            "to an existing device."
        )
    return device


current_module = sys.modules[__name__]


def _xpu(self, device=None, non_blocking=False, **kwargs):
    """Returns a copy of this object in xpu memory.

    If this object is already in xpu memory and on the correct device, then
    no copy is performed and the original object is returned.

    Args:
        device (int): The destination GPU id. Defaults to the current device.
        non_blocking (bool): If ``True`` and the source is in pinned memory,
            the copy will be asynchronous with respect to the host. Otherwise,
            the argument has no effect.
        **kwargs: For compatibility, may contain the key ``async`` in place of
            the ``non_blocking`` argument.
    """
    non_blocking = torch._utils._get_async_or_non_blocking("xpu", non_blocking, kwargs)
    # if self.is_xpu:
    #     if device is None:
    #         device = torch.xpu.current_device()
    #     if self.get_device() == device:
    #         return self
    # else:
    if device is None:
        device = -1
    with torch.xpu.device(device):
        if self.is_sparse:
            # new_type = getattr(torch.xpu.sparse, self.__class__.__name__)
            # indices = torch._indices(self).xpu(device, non_blocking)
            # values = torch._values(self).xpu(device, non_blocking)
            # return new_type(indices, values, self.size())
            pass
        else:
            untyped_storage = torch.UntypedStorage(
                self.size(), device=torch.device("xpu")
            )
            untyped_storage.copy_(self, non_blocking)
            return untyped_storage


def _xpu_deserialize(obj, location):
    if location.startswith("xpu"):
        device = validate_xpu_device(location)
        if getattr(obj, "_torch_load_uninitialized", False):
            with torch.xpu.device(device):
                return torch.UntypedStorage(obj.nbytes(), device=torch.device(location))
        else:
            return _xpu(obj, device=device)


_register_submodule_white_list = {
    "empty_cache",
    "max_memory_allocated",
    "max_memory_reserved",
    "memory_allocated",
    "memory_reserved",
    "memory_stats",
    "memory_stats_as_nested_dict",
    "reset_accumulated_memory_stats",
    "reset_peak_memory_stats",
}
_register_submodule_black_list = {"mem_get_info"}


def _register_torch_device_module(device_type, module):
    global _register_submodule_white_list
    global _register_submodule_black_list
    device_type = torch.device(device_type).type
    torch_module = sys.modules["torch"]
    torch_device_module = getattr(torch_module, device_type, None)
    torch_device_module_name = ".".join(["torch", device_type])
    if torch_device_module:
        for submodule_key in dir(module):
            if submodule_key in _register_submodule_black_list:
                continue
            if submodule_key in _register_submodule_white_list or not hasattr(
                torch_device_module, submodule_key
            ):
                submodule = getattr(module, submodule_key)
                setattr(
                    torch_device_module,
                    submodule_key,
                    submodule,
                )
                submodule_name = ".".join([torch_device_module_name, submodule_key])
                if isinstance(submodule, types.ModuleType):
                    sys.modules[submodule_name] = submodule
        sys.modules[torch_device_module_name] = torch_device_module
    else:
        torch._register_device_module(device_type, module)


if has_xpu():
    _StorageBase.xpu = _xpu

    serialization.register_package(30, _xpu_tag, _xpu_deserialize)

    _register_torch_device_module("xpu", current_module)

    # lazy init IPEX.
    _lazy_call(intel_extension_for_pytorch._C._initExtension)

    override_tensor_totype()
    exec_path = sys.argv[0].split("/")
    if len(exec_path) > 0 and "pytest" in exec_path:
        _lazy_call(override_assert_equal)

    from torch.utils.checkpoint import DefaultDeviceType

    DefaultDeviceType.set_device_type("xpu")
