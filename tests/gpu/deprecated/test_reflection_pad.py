import torch
import torch.nn as nn
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa


class TestNNMethod(TestCase):
    def test_reflection_pad1d(self, dtype=torch.float):
        x_cpu = torch.arange(8, dtype=dtype).reshape(1, 2, 4)
        grad_cpu = torch.arange(16, dtype=dtype).reshape(1, 2, 8)
        x_dpcpp = x_cpu.to("xpu")
        grad_dpcpp = grad_cpu.to("xpu")
        m = nn.ReflectionPad1d(2)

        x_cpu.requires_grad_(True)
        y_cpu = m(x_cpu)
        print("y_cpu", y_cpu)
        output_cpu = y_cpu.backward(grad_cpu)
        print("x_cpu.grad", x_cpu.grad)

        m.to("xpu")
        x_dpcpp.requires_grad_(True)
        y_dpcpp = m(x_dpcpp)
        print("y_dpcpp", y_dpcpp.to("cpu"))
        output_dpcpp = y_dpcpp.backward(grad_dpcpp)
        print("x_dpcpp.grad", x_dpcpp.grad.to("cpu"))

        self.assertEqual(y_cpu, y_dpcpp.to("cpu"))
        self.assertEqual(x_cpu.grad, x_dpcpp.grad.to("cpu"))

    def test_reflection_pad2d(self):
        for dtype in [torch.float32, torch.bfloat16]:
            x_cpu = torch.arange(2 * 3 * 6 * 8, dtype=dtype).reshape(2, 3, 6, 8)
            x_dpcpp = x_cpu.to("xpu")
            grad_cpu = torch.arange(2 * 3 * 9 * 11, dtype=dtype).reshape(2, 3, 9, 11)
            grad_dpcpp = grad_cpu.to("xpu")

            m = nn.ReflectionPad2d((1, 2, 3, 0))
            x_cpu.requires_grad_(True)
            y_cpu = m(x_cpu)
            print("y_cpu", y_cpu)
            output_cpu = y_cpu.backward(grad_cpu)
            print("x_cpu.grad", x_cpu.grad)

            m = nn.ReflectionPad2d((1, 2, 3, 0)).to("xpu")
            x_dpcpp.requires_grad_(True)
            y_dpcpp = m(x_dpcpp)
            print("y_dpcpp", y_dpcpp.to("cpu"))
            output_dpcpp = y_dpcpp.backward(grad_dpcpp)
            print("x_dpcpp.grad", x_dpcpp.grad.to("cpu"))

            self.assertEqual(y_cpu, y_dpcpp.to("cpu"))
            self.assertEqual(x_cpu.grad, x_dpcpp.grad.to("cpu"))

    def test_reflection_pad3d(self, dtype=torch.float):
        x_cpu = torch.arange(2 * 3 * 6 * 8, dtype=dtype).reshape(2, 3, 6, 8)
        x_dpcpp = x_cpu.to("xpu")
        grad_cpu = torch.arange(2 * 3 * 9 * 11, dtype=dtype).reshape(2, 3, 9, 11)
        grad_dpcpp = grad_cpu.to("xpu")

        m = nn.ReflectionPad3d((1, 2, 3, 0))
        x_cpu.requires_grad_(True)
        y_cpu = m(x_cpu)
        print("y_cpu", y_cpu)
        output_cpu = y_cpu.backward(grad_cpu)
        print("x_cpu.grad", x_cpu.grad)

        m = nn.ReflectionPad3d((1, 2, 3, 0)).to("xpu")
        x_dpcpp.requires_grad_(True)
        y_dpcpp = m(x_dpcpp)
        print("y_dpcpp", y_dpcpp.to("cpu"))
        output_dpcpp = y_dpcpp.backward(grad_dpcpp)
        print("x_dpcpp.grad", x_dpcpp.grad.to("cpu"))

        self.assertEqual(y_cpu, y_dpcpp.to("cpu"))
        self.assertEqual(x_cpu.grad, x_dpcpp.grad.to("cpu"))

    def test_reflection_pad1d_fwd(self):
        for dtype in [torch.int, torch.long, torch.float]:
            x_cpu = torch.arange(8, dtype=dtype).reshape(1, 2, 4)
            x_dpcpp = x_cpu.to("xpu")
            m = nn.ReflectionPad1d(2)

            y_cpu = m(x_cpu)

            m.to("xpu")
            y_dpcpp = m(x_dpcpp)

            self.assertEqual(y_cpu, y_dpcpp.to("cpu"))
