import torch
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa


cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestTensorMethod(TestCase):
    def test_tensordot(self, dtype=torch.float):
        a = torch.arange(60.0).reshape(3, 4, 5)
        b = torch.arange(24.0).reshape(4, 3, 2)
        c = torch.tensordot(a, b, dims=([1, 0], [0, 1]))
        print(c)

        print("---")
        a_dpcpp = a.to(dpcpp_device)
        b_dpcpp = b.to(dpcpp_device)
        c_dpcpp = torch.tensordot(a_dpcpp, b_dpcpp, dims=([1, 0], [0, 1]))
        print(c_dpcpp.to(cpu_device))

        self.assertEqual(c, c_dpcpp.to(cpu_device))
