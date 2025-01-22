from __future__ import print_function

import torch
import torch.nn.functional as F
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa


cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestNNMethod(TestCase):
    def test_nll_loss(self, dtype=torch.float):
        # input is of size N x C = 3 x 5
        input = torch.randn(3, 5)
        # each element in target has to have 0 <= value < C
        target = torch.tensor([1, 0, 4])
        x = torch.tensor((0.5), dtype=torch.float)
        input_dpcpp = input.to("xpu")
        target_dpcpp = target.to("xpu")
        x_dpcpp = x.to("xpu")
        input.requires_grad = True
        output = F.nll_loss(input, target)
        output.backward(x)
        print("CPU: ", output)
        print("CPU: ", input.grad)

        input_dpcpp.requires_grad = True
        output_dpcpp = F.nll_loss(input_dpcpp, target_dpcpp)
        output_dpcpp.backward(x_dpcpp)
        print("SYCL: ", output_dpcpp.to("cpu"))
        print("SYCL: ", input_dpcpp.grad.to("cpu"))
        self.assertEqual(output, output_dpcpp.cpu())
        self.assertEqual(input.grad, input_dpcpp.grad.cpu())

        # input is of size N x C x H x W = 3 x 5 x 2 x 2
        input = torch.randn(3, 5, 2, 2)
        # each element in target has to have 0 <= value < C
        target = torch.tensor([[[0, 1], [2, 3]], [[3, 2], [1, 0]], [[4, 1], [2, 3]]])
        input_dpcpp = input.to("xpu")
        target_dpcpp = target.to("xpu")

        input.requires_grad = True
        output = F.nll_loss(input, target, reduction="none")
        x = torch.ones([3, 2, 2], dtype=torch.float)
        output.backward(x)
        print("none CPU: ", output)
        print("none CPU grad: ", input.grad)
        input_dpcpp.requires_grad = True

        output_dpcpp = F.nll_loss(input_dpcpp, target_dpcpp, reduction="none")
        x_dpcpp = x.to("xpu")
        output_dpcpp.backward(x_dpcpp)
        print("none SYCL: ", output_dpcpp.to("cpu"))
        print("none SYCL grad: ", input_dpcpp.grad.to("cpu"))

        self.assertEqual(output, output_dpcpp.cpu())
        self.assertEqual(input.grad, input_dpcpp.grad.cpu())
        input.grad.detach_()
        input.grad.zero_()
        input_dpcpp.grad.detach_()
        input_dpcpp.grad.zero_()

        output = F.nll_loss(input, target, reduction="sum")
        x = torch.tensor((0.5), dtype=torch.float)
        output.backward(x)
        print("sum CPU: ", output)
        print("sum CPU grad: ", input.grad)

        output_dpcpp = F.nll_loss(input_dpcpp, target_dpcpp, reduction="sum")
        x_dpcpp = x.to("xpu")
        output_dpcpp.backward(x_dpcpp)
        print("sum SYCL: ", output_dpcpp.to("cpu"))
        print("sum SYCL grad: ", input_dpcpp.grad.to("cpu"))

        self.assertEqual(output, output_dpcpp.cpu())
        self.assertEqual(input.grad, input_dpcpp.grad.cpu())
        input.grad.detach_()
        input.grad.zero_()
        input_dpcpp.grad.detach_()
        input_dpcpp.grad.zero_()

        output = F.nll_loss(input, target, reduction="mean")
        output.backward(x)
        print("mean CPU: ", output)
        print("mean CPU grad: ", input.grad)

        output_dpcpp = F.nll_loss(input_dpcpp, target_dpcpp, reduction="mean")
        output_dpcpp.backward(x_dpcpp)
        print("mean SYCL: ", output_dpcpp.to("cpu"))
        print("mean SYCL grad: ", input_dpcpp.grad.to("cpu"))

        self.assertEqual(output, output_dpcpp.cpu())
        self.assertEqual(input.grad, input_dpcpp.grad.cpu())
        input.grad.detach_()
        input.grad.zero_()
        input_dpcpp.grad.detach_()
        input_dpcpp.grad.zero_()

    def test_nll_loss_half(self, dtype=torch.float16):
        # input is of size N x C = 3 x 5
        input = torch.randn(3, 5)
        # each element in target has to have 0 <= value < C
        target = torch.tensor([1, 0, 4])
        x = torch.tensor((0.5), dtype=torch.float)
        input_dpcpp = input.to("xpu").to(dtype)
        target_dpcpp = target.to("xpu")
        x_dpcpp = x.to("xpu").to(dtype)
        input.requires_grad = True
        output = F.nll_loss(input, target)
        output.backward(x)
        print("CPU: ", output)
        print("CPU: ", input.grad)

        input_dpcpp.requires_grad = True
        output_dpcpp = F.nll_loss(input_dpcpp, target_dpcpp)
        output_dpcpp.backward(x_dpcpp)
        print("SYCL: ", output_dpcpp.to("cpu"))
        print("SYCL: ", input_dpcpp.grad.to("cpu"))
        self.assertEqual(output, output_dpcpp.cpu().float(), rtol=1e-03, atol=1e-03)
        self.assertEqual(
            input.grad, input_dpcpp.grad.cpu().float(), rtol=1e-03, atol=1e-03
        )

        input = torch.randn(3, 5, 4)
        # each element in target has to have 0 <= value < C
        target = torch.tensor([[0, 1, 2, 3], [3, 2, 1, 0], [4, 1, 2, 3]])
        input_dpcpp = input.to("xpu").to(dtype)
        target_dpcpp = target.to("xpu")

        input.requires_grad = True
        output = F.nll_loss(input, target, reduction="none")
        x = torch.ones([3, 4], dtype=torch.float)
        output.backward(x)
        print("none CPU: ", output)
        print("none CPU grad: ", input.grad)
        input_dpcpp.requires_grad = True

        output_dpcpp = F.nll_loss(input_dpcpp, target_dpcpp, reduction="none")
        x_dpcpp = x.to("xpu").to(dtype)
        output_dpcpp.backward(x_dpcpp)
        print("none SYCL: ", output_dpcpp.to("cpu"))
        print("none SYCL grad: ", input_dpcpp.grad.to("cpu"))

        self.assertEqual(output, output_dpcpp.cpu().float(), rtol=1e-03, atol=1e-03)
        self.assertEqual(
            input.grad, input_dpcpp.grad.cpu().float(), rtol=1e-03, atol=1e-03
        )
        input.grad.detach_()
        input.grad.zero_()
        input_dpcpp.grad.detach_()
        input_dpcpp.grad.zero_()

        output = F.nll_loss(input, target, reduction="sum")
        x = torch.tensor((0.5), dtype=torch.float)
        output.backward(x)
        print("sum CPU: ", output)
        print("sum CPU grad: ", input.grad)

        output_dpcpp = F.nll_loss(input_dpcpp, target_dpcpp, reduction="sum")
        x_dpcpp = x.to("xpu")
        output_dpcpp.backward(x_dpcpp)
        print("sum SYCL: ", output_dpcpp.to("cpu"))
        print("sum SYCL grad: ", input_dpcpp.grad.to("cpu"))

        self.assertEqual(output, output_dpcpp.cpu().float(), rtol=1e-03, atol=1e-03)
        self.assertEqual(
            input.grad, input_dpcpp.grad.cpu().float(), rtol=1e-03, atol=1e-03
        )
        input.grad.detach_()
        input.grad.zero_()
        input_dpcpp.grad.detach_()
        input_dpcpp.grad.zero_()

        output = F.nll_loss(input, target, reduction="mean")
        output.backward(x)
        print("mean CPU: ", output)
        print("mean CPU grad: ", input.grad)

        output_dpcpp = F.nll_loss(input_dpcpp, target_dpcpp, reduction="mean")
        output_dpcpp.backward(x_dpcpp)
        print("mean SYCL: ", output_dpcpp.to("cpu"))
        print("mean SYCL grad: ", input_dpcpp.grad.to("cpu"))

        self.assertEqual(output, output_dpcpp.cpu().float(), rtol=1e-03, atol=1e-03)
        self.assertEqual(
            input.grad, input_dpcpp.grad.cpu().float(), rtol=1e-03, atol=1e-03
        )
        input.grad.detach_()
        input.grad.zero_()
        input_dpcpp.grad.detach_()
        input_dpcpp.grad.zero_()

    def test_nll_loss_corner_case(self, dtype=torch.float16):
        def fn_forward(a, b1):
            return torch.ops.aten.nll_loss_forward(a, b1, None, 1, -100)

        labels = (
            torch.zeros([5], dtype=torch.int64, device="xpu"),
            torch.tensor([-100, -100, 3, -100, -100], dtype=torch.int64, device="xpu"),
        )
        inps = (torch.randn(5, 5, device="xpu"), torch.randn(5, 5, device="xpu"))

        for a, b in zip(inps, labels):
            fn_forward(a, b)

        def fn_backward(a, b, c):
            return torch.ops.aten.nll_loss_backward(
                a, b, c, None, 1, -100, torch.tensor(1.0, device="xpu")
            )

        grad_outs = (torch.randn((), device="xpu"), torch.randn((), device="xpu"))
        for a, b, c in zip(grad_outs, inps, labels):
            fn_backward(a, b, c)
