import torch
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa


cpu_device = torch.device("cpu")
sycl_device = torch.device("xpu")


class TestTorchMethod(TestCase):
    def test_index_randperm(self):
        src = torch.empty(150, 45, device=sycl_device).random_(0, 2**22)
        idx = torch.randperm(src.shape[0], device=sycl_device)
        res = src[idx]
        res_cpu = src.cpu()[idx.cpu()]
        self.assertEqual(res.cpu(), res_cpu)

    def test_corner_randperm(self):
        res_cpu = torch.randperm(0, device=cpu_device)[:0]
        res_xpu = torch.randperm(0, device=sycl_device)[:0]
        self.assertEqual(res_cpu, res_xpu.to(cpu_device))
