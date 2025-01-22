import torch
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa

cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestTorchMethod(TestCase):
    def test_median(self, dtype=torch.float):
        x_cpu = torch.randn(2, 3)
        x_dpcpp = x_cpu.to("xpu")

        print("x_cpu", x_cpu, " median_cpu", torch.median(x_cpu))
        print(
            "x_dpcpp",
            x_dpcpp.to("cpu"),
            " median_dpcpp",
            torch.median(x_dpcpp).to("cpu"),
        )
        self.assertEqual(x_cpu, x_dpcpp.cpu())
        self.assertEqual(torch.median(x_cpu), torch.median(x_dpcpp).to("cpu"))

        x_cpu2 = torch.tensor(
            ([1, 2, 3, 4, 5]), dtype=torch.int32, device=torch.device("cpu")
        )
        x_dpcpp2 = torch.tensor(
            ([1, 2, 3, 4, 5]), dtype=torch.int32, device=torch.device("xpu")
        )

        print("x_cpu2", x_cpu2, " median_cpu2", x_cpu2.median())
        print(
            "x_dpcpp2",
            x_dpcpp2.to("cpu"),
            " median_dpcpp2",
            x_dpcpp2.median().to("cpu"),
        )
        self.assertEqual(x_cpu2, x_dpcpp2.cpu())
        self.assertEqual(torch.median(x_cpu2), torch.median(x_dpcpp2).to("cpu"))
        x_cpu3 = torch.tensor([1, 3, 5, float("nan"), 2, 4, 6])
        x_dpcpp3 = x_cpu3.clone().to(dpcpp_device)
        self.assertEqual(torch.median(x_cpu3), torch.median(x_dpcpp3).to(cpu_device))

    def test_nanmedian(self, dtype=torch.float):
        x_base = torch.randn(100)
        x_nan = torch.tensor([1.0, 2.0, float("nan"), 0.5])
        x_cpu = torch.cat((x_base, x_nan))
        x_dpcpp = x_cpu.clone().to(dpcpp_device)
        self.assertEqual(
            torch.nanmedian(x_cpu), torch.nanmedian(x_dpcpp).to(cpu_device)
        )
