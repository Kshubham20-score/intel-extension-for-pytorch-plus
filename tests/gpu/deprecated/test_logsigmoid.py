import torch
import torch.nn as nn
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa


cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestNNMethod(TestCase):
    def test_logsigmoid(self, dtype=torch.float):
        # cpu
        linear = nn.Linear(8, 8)
        tanh = nn.LogSigmoid()

        print("linear weight", linear.weight)

        x_cpu = torch.ones([1, 1, 8, 8], device=cpu_device, dtype=dtype)
        print("x_cpu", x_cpu)

        z_cpu = linear(x_cpu)
        print("z_cpu", z_cpu)

        y_cpu = tanh(z_cpu)
        print("y_cpu", y_cpu)

        y_cpu.backward(torch.ones([1, 1, 8, 8], device=cpu_device))
        print("linear grad", linear.weight.grad)

        # dpcpp
        linear_dpcpp = linear.to("xpu")
        tanh_dpcpp = tanh.to("xpu")

        print("dpcpp linear weight", linear_dpcpp.weight.cpu())

        x_dpcpp = x_cpu.to("xpu")
        print("x_dpcpp", x_dpcpp.cpu())

        z_dpcpp = linear_dpcpp(x_dpcpp)
        print("z_dpcpp", z_dpcpp.cpu())

        y_dpcpp = tanh(z_dpcpp)
        print("y_dpcpp", y_dpcpp.cpu())

        y_dpcpp.backward(torch.ones([1, 1, 8, 8], device=dpcpp_device))
        print("dpcpp linear grad", linear_dpcpp.weight.grad.cpu())
        self.assertEqual(x_cpu, x_dpcpp.cpu())
        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(z_cpu, z_dpcpp.cpu())
        self.assertEqual(linear.weight, linear_dpcpp.weight.cpu())
        self.assertEqual(linear.weight.grad, linear_dpcpp.weight.grad.cpu())

    def test_logsigmoid_half(self, dtype=torch.float16):
        grad_output_cpu = torch.randn([1, 1, 8, 8])
        grad_output_xpu = grad_output_cpu.to("xpu", dtype=dtype)

        # cpu
        linear = nn.Linear(8, 8)
        tanh = nn.LogSigmoid()
        x_cpu = torch.ones([1, 1, 8, 8], device=cpu_device, dtype=torch.float32)
        z_cpu = linear(x_cpu)
        y_cpu = tanh(z_cpu)
        y_cpu.backward(grad_output_cpu)
        linear_weight_grad_cpu = linear.weight.grad.clone()
        linear.zero_grad()

        # xpu
        linear_xpu = linear.to("xpu", dtype=dtype)
        tanh_xpu = tanh.to("xpu", dtype=dtype)
        x_xpu = x_cpu.to("xpu", dtype=dtype)
        z_xpu = linear_xpu(x_xpu)
        y_xpu = tanh_xpu(z_xpu)
        y_xpu.backward(grad_output_xpu)
        linear_weight_grad_xpu = linear.weight.grad.clone()
        linear_xpu.zero_grad()

        self.assertEqual(
            z_cpu, z_xpu.to("cpu", dtype=torch.float32), atol=1e-3, rtol=1e-3
        )
        self.assertEqual(
            y_cpu, y_xpu.to("cpu", dtype=torch.float32), atol=1e-3, rtol=1e-3
        )
        self.assertEqual(
            linear_weight_grad_cpu,
            linear_weight_grad_xpu.to("cpu", dtype=torch.float32),
            atol=1e-3,
            rtol=1e-3,
        )
