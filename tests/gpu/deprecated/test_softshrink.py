from __future__ import print_function

import torch
import torch.nn.functional as F
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa

cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestTorchMethod(TestCase):
    def test_softshrink(self, dtype=torch.float):
        x_cpu = torch.tensor(
            [[0.5, 1.5, 0.1], [2.2, 1.2, 1.7]], requires_grad=True, device=cpu_device
        )
        y_cpu_output = torch.tensor(
            [[0.5, 1.5, 0.1], [2.2, 1.2, 1.7]], requires_grad=True, device=cpu_device
        )

        y = F.softshrink(x_cpu, 1)
        y.backward(y_cpu_output)

        print("CPU Result:")
        print("x:", x_cpu)
        print("softshrink:", y)
        print("softshrink x_cpu_grad = ", x_cpu.grad)

        x_dpcpp = torch.tensor(
            [[0.5, 1.5, 0.1], [2.2, 1.2, 1.7]], requires_grad=True, device=dpcpp_device
        )
        y_dpcpp_output = torch.tensor(
            [[0.5, 1.5, 0.1], [2.2, 1.2, 1.7]], requires_grad=True, device=dpcpp_device
        )

        y_dpcpp = F.softshrink(x_dpcpp, 1)
        y_dpcpp.backward(y_dpcpp_output)

        print("SYCL Result:")
        print("x_dpcpp:", x_dpcpp.cpu())
        print("softshrink dpcpp:", y_dpcpp.cpu())
        print("softshrink x_dpcpp_grad = ", x_dpcpp.grad.cpu())
        self.assertEqual(x_cpu, x_dpcpp.cpu())
        self.assertEqual(y, y_dpcpp.cpu())
        self.assertEqual(x_cpu.grad, x_dpcpp.grad.cpu())

    def test_softshrink_half(self, dtype=torch.float16):
        x_cpu = torch.ones(
            [1, 1, 8, 8], device=cpu_device, requires_grad=True, dtype=dtype
        )
        x_xpu = x_cpu.clone().detach().to("xpu").requires_grad_()
        y_cpu = F.softshrink(x_cpu, 1)
        z_cpu = y_cpu.mean()
        z_cpu.backward()

        y_xpu = F.softshrink(x_xpu, 1)
        z_xpu = y_xpu.mean()
        z_xpu.backward()

        self.assertEqual(x_cpu.grad, x_xpu.grad.cpu())
