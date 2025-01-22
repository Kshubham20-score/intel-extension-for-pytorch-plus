import torch
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa


cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestTensorMethod(TestCase):
    def test_addcdiv(self, dtype=torch.float):
        x = torch.tensor(
            [[0.6580, -1.0969, -0.4614], [-0.1034, -0.5790, 0.1497]], device=cpu_device
        )
        # x_dpcpp = torch.tensor([[ 0.6580, -1.0969, -0.4614], [-0.1034, -0.5790,  0.1497]], device = dpcpp_device)
        x_dpcpp = x.to("xpu")

        y = torch.addcdiv(x, 0.1, x, x)

        print("addcdiv cpu:", y)
        y_dpcpp = torch.addcdiv(x_dpcpp, 0.1, x_dpcpp, x_dpcpp)
        print("addcdiv dpcpp:", y_dpcpp.cpu())
        self.assertEqual(y, y_dpcpp.cpu())

        x.addcdiv_(0.1, x, x)
        print("addcdiv_ cpu:", x)
        x_dpcpp.addcdiv_(0.1, x_dpcpp, x_dpcpp)
        print("addcdiv_ dpcpp:", x_dpcpp.cpu())
        self.assertEqual(x, x_dpcpp.cpu())

    def test_addcdiv_bf16(self, dtype=torch.float):
        x = torch.tensor(
            [[0.6580, -1.0969, -0.4614], [-0.1034, -0.5790, 0.1497]], device=cpu_device
        )
        # x_dpcpp = torch.tensor([[ 0.6580, -1.0969, -0.4614], [-0.1034, -0.5790,  0.1497]], device = dpcpp_device)
        x_dpcpp = x.to("xpu").to(torch.bfloat16)

        y = torch.addcdiv(x, 0.1, x, x)

        print("addcdiv cpu:", y)
        y_dpcpp = torch.addcdiv(x_dpcpp, 0.1, x_dpcpp, x_dpcpp)
        print("addcdiv dpcpp:", y_dpcpp.cpu())
        # self.assertEqual(y, y_dpcpp.cpu(), prec=0.1)

        x.addcdiv_(0.1, x, x)
        print("addcdiv_ cpu:", x)
        x_dpcpp.addcdiv_(0.1, x_dpcpp, x_dpcpp)
        print("addcdiv_ dpcpp:", x_dpcpp.cpu())
        # self.assertEqual(x, x_dpcpp.cpu(), prec=0.1)

    def test_addcmul(self, dtype=torch.float):
        x = torch.tensor(
            [[0.6580, -1.0969, -0.4614], [-0.1034, -0.5790, 0.1497]], device=cpu_device
        )
        # x_dpcpp = torch.tensor([[ 0.6580, -1.0969, -0.4614], [-0.1034, -0.5790,  0.1497]], device = dpcpp_device)
        x_dpcpp = x.to("xpu")

        y = torch.addcmul(x, 0.1, x, x)

        print("addcmul cpu:", y)
        y_dpcpp = torch.addcmul(x_dpcpp, 0.1, x_dpcpp, x_dpcpp)
        print("addcmul dpcpp: ", y_dpcpp.cpu())
        self.assertEqual(y, y_dpcpp.cpu())

        x.addcmul_(0.1, x, x)
        print("addcmul_ cpu:", x)
        x_dpcpp.addcmul_(0.1, x_dpcpp, x_dpcpp)
        print("addcmul_ dpcpp:", x_dpcpp.cpu())
        self.assertEqual(x, x_dpcpp.cpu())

    def test_addcmul_bf16(self, dtype=torch.float):
        x = torch.tensor(
            [[0.6580, -1.0969, -0.4614], [-0.1034, -0.5790, 0.1497]], device=cpu_device
        )
        # x_dpcpp = torch.tensor([[ 0.6580, -1.0969, -0.4614], [-0.1034, -0.5790,  0.1497]], device = dpcpp_device)
        x_dpcpp = x.to("xpu").to(torch.bfloat16)

        y = torch.addcmul(x, 0.1, x, x)

        print("addcmul cpu:", y)
        y_dpcpp = torch.addcmul(x_dpcpp, 0.1, x_dpcpp, x_dpcpp)
        print("addcmul dpcpp: ", y_dpcpp.cpu())
        # self.assertEqual(y, y_dpcpp.cpu(), prec=0.1)

        x.addcmul_(0.1, x, x)
        print("addcmul_ cpu:", x)
        x_dpcpp.addcmul_(0.1, x_dpcpp, x_dpcpp)
        print("addcmul_ dpcpp:", x_dpcpp.cpu())
        # self.assertEqual(x, x_dpcpp.cpu(), prec=0.1)

    def test_lerp(self, dtype=torch.float):
        x = torch.tensor(
            [[0.6580, -1.0969, -0.4614], [-0.1034, -0.5790, 0.1497]], device=cpu_device
        )
        # x_dpcpp = torch.tensor([[ 0.6580, -1.0969, -0.4614], [-0.1034, -0.5790,  0.1497]], device = dpcpp_device)
        x_dpcpp = x.to("xpu")

        y = torch.lerp(x, x, 0.5)
        y_dpcpp = torch.lerp(x_dpcpp, x_dpcpp, 0.5)
        print("lerp cpu:", y)
        print("lerp dpcpp: ", y_dpcpp.cpu())
        self.assertEqual(y, y_dpcpp.cpu())
