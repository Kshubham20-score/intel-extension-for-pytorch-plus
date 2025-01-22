import torch
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa


cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestNNMethod(TestCase):
    def test_random_norm(self, dtype=torch.float):
        x_cpu = torch.tensor(list(range(1000)), device=cpu_device, dtype=dtype)
        x_dpcpp = torch.tensor(list(range(1000)), device=dpcpp_device, dtype=dtype)

        print("normal_ cpu", x_cpu.normal_(2.0, 0.5))
        print("normal_ dpcpp", x_dpcpp.normal_(2.0, 0.5).cpu())

        self.assertEqual(x_dpcpp.mean(), 2.0, rtol=0.3, atol=0.3)
        self.assertEqual(x_dpcpp.std(), 0.5, rtol=0.3, atol=0.3)
