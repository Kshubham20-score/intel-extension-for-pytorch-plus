import torch
import random
from torch.testing._internal.common_utils import TestCase
import intel_extension_for_pytorch  # noqa


class TestNNMethod(TestCase):
    def test_sort(self):
        for dtype in [torch.bfloat16, torch.half, torch.bool]:
            for i in range(100):
                a = random.randint(1, 3)
                b = random.randint(1, 5)
                c = random.randint(1, 7)
                d = random.randint(1, 26384)
                abcd = [a, b, c, d]
                random.shuffle(abcd)
                x_cpu = torch.randn(abcd).to(dtype)
                x_cpu = torch.testing._internal.common_utils.noncontiguous_like(x_cpu)
                x_xpu = x_cpu.clone().xpu()
                x_xpu = torch.testing._internal.common_utils.noncontiguous_like(x_xpu)
                dim = random.randint(0, 3)
                descending = b % 2 == 0
                x_cpu = torch.sort(x_cpu, dim=dim, descending=descending)[0]
                x_xpu = torch.sort(x_xpu, dim=dim, descending=descending)[0]
                maxdiff = float((x_cpu.float() - x_xpu.cpu().float()).abs().max())
                print(abcd, ", dim:", dim, ", maxdiff:", maxdiff)
                assert maxdiff < 1e-5

    def test_sort_focus_case_unstable(self, dtype=torch.float):
        x = torch.randn(4, 8193, 3)
        y = x.to("xpu")
        res_cpu, _ = torch.sort(x, dim=1)
        res, _ = torch.sort(y, dim=1)
        self.assertEqual(res_cpu, res.cpu())

        x = torch.randn(4, 2049, 3)
        y = x.to("xpu")
        res_cpu, _ = torch.sort(x, dim=1)
        res, _ = torch.sort(y, dim=1)
        self.assertEqual(res_cpu, res.cpu())

        x = torch.randn(4, 511, 3)
        y = x.to("xpu")
        res_cpu, _ = torch.sort(x, dim=1)
        res, _ = torch.sort(y, dim=1)
        self.assertEqual(res_cpu, res.cpu())

    def test_sort_focus_case_stable(self, dtype=torch.float):
        x = torch.randn(4, 8193, 3)
        y = x.to("xpu")
        res_cpu, index_cpu = torch.sort(x, dim=1, stable=True)
        res, index = torch.sort(y, dim=1, stable=True)
        self.assertEqual(res_cpu, res.cpu())
        self.assertEqual(index_cpu, index.cpu())

        x = torch.randn(4, 2049, 3)
        y = x.to("xpu")
        res_cpu, index_cpu = torch.sort(x, dim=1, stable=True)
        res, index = torch.sort(y, dim=1, stable=True)
        self.assertEqual(res_cpu, res.cpu())
        self.assertEqual(index_cpu, index.cpu())

        x = torch.randn(4, 511, 3)
        y = x.to("xpu")
        res_cpu, index_cpu = torch.sort(x, dim=1, stable=True)
        res, index = torch.sort(y, dim=1, stable=True)
        self.assertEqual(res_cpu, res.cpu())
        self.assertEqual(index_cpu, index.cpu())

    def test_argsort(self):
        x = torch.Tensor(
            [
                [0.0785, 1.5267, -0.8521, 0.4065],
                [0.1598, 0.0788, -0.0745, -1.2700],
                [1.2208, 1.0722, -0.7064, 1.2564],
                [0.0669, -0.2318, -0.8229, -0.9280],
            ]
        )
        result = torch.Tensor([[2, 0, 3, 1], [3, 2, 1, 0], [2, 1, 0, 3], [3, 2, 1, 0]])
        self.assertEqual(torch.argsort(x, dim=1).int(), result.int())

        x1 = torch.Tensor([[2, 0, 1], [0, 1, 2]])
        result1 = torch.Tensor([[1, 0, 0], [0, 1, 1]])
        result1_des = torch.Tensor([[0, 1, 1], [1, 0, 0]])
        self.assertEqual(torch.argsort(x1, dim=0).int(), result1.int())
        self.assertEqual(
            torch.argsort(x1, dim=0, descending=False).int(), result1.int()
        )
