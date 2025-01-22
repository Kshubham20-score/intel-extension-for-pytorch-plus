import torch
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa

cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestTorchMethod(TestCase):
    def test_masked_fill(self, dtype=torch.float):
        x_cpu = torch.tensor(
            [[1, 2, 3, 4]], device=torch.device("cpu"), dtype=torch.float
        )
        x_dpcpp = torch.tensor(
            [[1, 2, 3, 4]], device=torch.device("xpu"), dtype=torch.float
        )

        y_cpu = x_cpu.masked_fill(
            mask=torch.BoolTensor([True, True, False, False]), value=torch.tensor(-1e9)
        )
        y_dpcpp = x_dpcpp.masked_fill(
            mask=torch.BoolTensor([True, True, False, False]).to("xpu"),
            value=torch.tensor(-1e9),
        )

        print("y_cpu = ", y_cpu)
        print("y_dpcpp = ", y_dpcpp.cpu())
        self.assertEqual(y_cpu, y_dpcpp.cpu())
