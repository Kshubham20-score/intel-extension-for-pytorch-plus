import torch
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa

dpcpp_device = torch.device("xpu")


class TestTorchMethod(TestCase):
    def test_aminmax(self, dtype=torch.float):
        # Test aminmax without dim
        src_cpu = torch.randn(2, 5)
        dst_cpu = torch.aminmax(src_cpu)

        src_gpu = src_cpu.to(dpcpp_device)
        dst_gpu = torch.aminmax(src_gpu)

        self.assertEqual(dst_cpu, dst_gpu)

        # Test aminmax with dim
        src_cpu = torch.randn(2, 5)
        dst_cpu = torch.aminmax(src_cpu, dim=0)

        src_gpu = src_cpu.to(dpcpp_device)
        dst_gpu = torch.aminmax(src_gpu, dim=0)

        self.assertEqual(dst_cpu, dst_gpu)

    def test_aminmax_bool(self, dtype=torch.bool):
        # Test aminmax without dim
        src_cpu = torch.tensor([True, False, True, False], dtype=dtype)
        dst_cpu = torch.aminmax(src_cpu)

        src_gpu = src_cpu.to(dpcpp_device)
        dst_gpu = torch.aminmax(src_gpu)

        self.assertEqual(dst_cpu, dst_gpu)

        # Test aminmax with dim
        src_cpu = torch.tensor([True, False, True, False], dtype=dtype)
        dst_cpu = torch.aminmax(src_cpu, dim=0)

        src_gpu = src_cpu.to(dpcpp_device)
        dst_gpu = torch.aminmax(src_gpu, dim=0)

        self.assertEqual(dst_cpu, dst_gpu)
