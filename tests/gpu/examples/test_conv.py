import torch
import torch.nn as nn
from torch.testing._internal.common_utils import TestCase
import intel_extension_for_pytorch  # noqa
import pytest

cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")

# Note:
# In order to press the gradient of weight below 1,
# the default weight should be set to 1e-ks (ks is kernel_size).
# For now, precision could not be pressed to 1e-5,
# but only if there is a real model which suffers the accuracy problem,
# we won't delve into this issue.


class TestNNMethod(TestCase):
    def test_conv2d_first(self, dtype=torch.float):
        x_cpu = torch.randn(
            [1, 3, 256, 256], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [1, 64, 256, 256], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv2d(3, 64, kernel_size=3, stride=1, padding=1, bias=False)
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv2d_strided(self, dtype=torch.float):
        x_cpu = torch.randn(
            [1, 3, 256, 256], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [1, 64, 256, 256], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv2d(3, 64, kernel_size=3, stride=1, padding=1, bias=False)
        x_cpu = x_cpu.transpose(2, 3)
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv2d(self, dtype=torch.float):
        x_cpu = torch.randn(
            [1, 64, 256, 256], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [1, 64, 256, 256], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv2d(64, 64, kernel_size=3, stride=1, padding=1, bias=False)
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv2d_half(self, dtype=torch.float16):
        x = torch.randn(
            [1, 64, 256, 256], dtype=dtype, device=cpu_device, requires_grad=True
        ).to(dpcpp_device)
        grad = torch.full(
            [1, 64, 256, 256], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        ).to(dpcpp_device)
        conv = (
            nn.Conv2d(64, 64, kernel_size=3, stride=1, padding=1, bias=False)
            .to(dpcpp_device)
            .half()
        )
        y_fp32 = conv(x)
        y_fp32.backward(grad)
        y_gw = conv.weight.grad.detach().clone()

        conv.zero_grad()

        x_fp16 = x.to(dtype).requires_grad_()
        grad_fp16 = grad.to(dtype)
        y_fp16 = conv(x_fp16)
        y_fp16.backward(grad_fp16)
        y_fp16_gw = conv.weight.grad.detach().clone()

        self.assertEqual(y_fp16, y_fp32, atol=1e-3, rtol=1e-3)
        self.assertEqual(y_gw, y_fp16_gw, atol=1e-3, rtol=1e-3)

    def test_Conv2d_deterministic(self, dtype=torch.float32):
        device = "xpu"
        with torch.backends.mkldnn.flags(deterministic=True):
            inputs = torch.randn(
                2, 2064, 32, 32, device=device, dtype=dtype, requires_grad=True
            )
            conv1 = torch.nn.Conv2d(2064, 3, 3).to(device, dtype)
            conv2 = torch.nn.Conv2d(2064, 3, 3).to(device, dtype)
            conv2.bias.data.copy_(conv1.bias.data)
            conv2.weight.data.copy_(conv1.weight.data)
            out1 = conv1(inputs)
            out2 = conv2(inputs)
            self.assertEqual(out1, out2, atol=0.0, rtol=0)
            y = torch.randn(out1.size(), device=device, dtype=dtype)
            out1.backward(y)
            out2.backward(y)
        self.assertEqual(conv1.bias.grad.data, conv2.bias.grad.data, atol=0.0, rtol=0)
        self.assertEqual(
            conv1.weight.grad.data, conv2.weight.grad.data, atol=0.0, rtol=0
        )

    @pytest.mark.skipif(
        not torch.xpu.has_fp64_dtype(), reason="fp64 not support by this device"
    )
    def test_conv2d_double(self, dtype=torch.double):
        x_cpu = torch.randn(
            [1, 64, 256, 256], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [1, 64, 256, 256], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv2d(
            64, 64, kernel_size=3, stride=1, padding=1, bias=False
        ).double()
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv2d_with_bias(self, dtype=torch.float):
        x_cpu = torch.randn(
            [1, 64, 256, 256], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [1, 64, 256, 256], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv2d(64, 64, kernel_size=3, stride=1, padding=1, bias=True)
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv2d_dilated(self, dtype=torch.float):
        x_cpu = torch.randn(
            [1, 64, 256, 256], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [1, 64, 254, 254], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv2d(
            64, 64, kernel_size=3, stride=1, padding=1, dilation=2, bias=False
        )
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv2d_dilated_with_bias(self, dtype=torch.float):
        x_cpu = torch.randn(
            [1, 64, 256, 256], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [1, 64, 254, 254], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv2d(
            64, 64, kernel_size=3, stride=1, padding=1, dilation=2, bias=True
        )
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv3d(self, dtype=torch.float):
        x_cpu = torch.randn(
            [2, 16, 10, 128, 128], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [2, 32, 10, 128, 128],
            1e-3,
            dtype=dtype,
            device=cpu_device,
            requires_grad=True,
        )
        conv_cpu = nn.Conv3d(16, 32, kernel_size=3, stride=1, padding=1, bias=False)
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv3d_channels_last(self, dtype=torch.float):
        x_cpu = torch.randn(
            [2, 16, 10, 128, 128], dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv3d(16, 32, kernel_size=3, stride=1, padding=1, bias=False)
        y_cpu = conv_cpu(x_cpu)

        x_dpcpp = x_cpu.to(dpcpp_device).to(memory_format=torch.channels_last_3d)
        conv_dpcpp = conv_cpu.to(dpcpp_device).to(memory_format=torch.channels_last_3d)
        y_dpcpp = conv_dpcpp(x_dpcpp)

        self.assertEqual(y_cpu, y_dpcpp.cpu())

    def test_conv3d_with_bias(self, dtype=torch.float):
        x_cpu = torch.randn(
            [2, 16, 10, 128, 128], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [2, 32, 10, 128, 128],
            1e-3,
            dtype=dtype,
            device=cpu_device,
            requires_grad=True,
        )
        conv_cpu = nn.Conv3d(16, 32, kernel_size=3, stride=1, padding=1, bias=True)
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv3d_dilated(self, dtype=torch.float):
        x_cpu = torch.randn(
            [2, 16, 10, 128, 128], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [2, 32, 6, 124, 124],
            1e-3,
            dtype=dtype,
            device=cpu_device,
            requires_grad=True,
        )
        conv_cpu = nn.Conv3d(
            16, 32, kernel_size=3, stride=1, padding=1, dilation=3, bias=False
        )
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv3d_dilated_with_bias(self, dtype=torch.float):
        x_cpu = torch.randn(
            [2, 16, 10, 128, 128], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [2, 32, 6, 124, 124],
            1e-3,
            dtype=dtype,
            device=cpu_device,
            requires_grad=True,
        )
        conv_cpu = nn.Conv3d(
            16, 32, kernel_size=3, stride=1, padding=1, dilation=3, bias=True
        )
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv_with_nosquare_kernel_size(self, dtype=torch.float):
        x_cpu = torch.randn(
            [20, 16, 50, 100], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [20, 33, 26, 100], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv2d(
            16,
            33,
            kernel_size=(3, 5),
            stride=(2, 1),
            padding=(4, 2),
            dilation=(3, 1),
            bias=True,
        )
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.cpu())
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_primitive_cache(self, dtype=torch.float):
        x_cpu = torch.randn(
            [1, 2, 3, 3], dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv1_cpu = nn.Conv2d(2, 2, kernel_size=3, stride=1, padding=1, bias=False)
        conv2_cpu = nn.Conv2d(2, 2, kernel_size=3, stride=1, padding=1, bias=False)
        conv3_cpu = nn.Conv2d(2, 3, kernel_size=3, stride=1, padding=1, bias=False)
        conv4_cpu = nn.Conv2d(3, 3, kernel_size=3, stride=1, padding=1, bias=False)
        conv5_cpu = nn.Conv2d(3, 3, kernel_size=3, stride=1, padding=1, bias=False)
        conv6_cpu = nn.Conv2d(3, 2, kernel_size=3, stride=1, padding=1, bias=False)
        y_cpu = conv6_cpu(conv5_cpu(conv4_cpu(conv3_cpu(conv2_cpu(conv1_cpu(x_cpu))))))

        conv1 = conv1_cpu.to("xpu")
        conv2 = conv2_cpu.to("xpu")
        conv3 = conv3_cpu.to("xpu")
        conv4 = conv4_cpu.to("xpu")
        conv5 = conv5_cpu.to("xpu")
        conv6 = conv6_cpu.to("xpu")
        x = x_cpu.to("xpu")
        y = conv6(conv5(conv4(conv3(conv2(conv1(x))))))

        self.assertEqual(y_cpu, y.cpu())

    def test_group_conv(self, dtype=torch.float):
        conv = nn.Conv2d(
            256, 64, kernel_size=3, stride=1, padding=1, bias=False, groups=2
        ).to(dpcpp_device)
        x = torch.randn(
            [1, 256, 3, 3], dtype=torch.float, device=cpu_device, requires_grad=True
        ).to(dpcpp_device)
        grad = torch.full(
            [1, 64, 3, 3],
            1e-3,
            dtype=torch.float,
            device=cpu_device,
            requires_grad=True,
        ).to(dpcpp_device)
        real = conv(x)
        real.backward(grad)
        y_dpcpp_gw = conv.weight.grad.detach().clone()

        conv.zero_grad()

        conv_cpu = conv.cpu()
        x_cpu = x.cpu()
        grad_cpu = grad.cpu()
        ref = conv_cpu(x_cpu)
        ref.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        self.assertEqual(real.cpu(), ref)
        self.assertEqual(y_dpcpp_gw.cpu(), y_cpu_gw)

    def test_conv2d_blk_with_h1w1(self, dtype=torch.float):
        x_cpu = torch.randn(
            [128, 256, 1, 1], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [128, 1, 1, 1], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv2d(256, 1, kernel_size=(1, 1), stride=(1, 1))
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        with torch.xpu.onednn_layout():
            x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
            grad_dpcpp = grad_cpu.to(dpcpp_device)
            conv_dpcpp = conv_cpu.to(dpcpp_device)
            y_dpcpp = conv_dpcpp(x_dpcpp)
            y_dpcpp.backward(grad_dpcpp)
            y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

            self.assertEqual(y_cpu, y_dpcpp.cpu())
            self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu())

    def test_group_conv_blk(self, dtype=torch.float):
        conv = nn.Conv2d(
            256, 64, kernel_size=3, stride=1, padding=1, bias=False, groups=2
        ).to(cpu_device)
        x = torch.randn(
            [1, 256, 3, 3], dtype=torch.float, device=cpu_device, requires_grad=True
        ).to(cpu_device)
        grad = torch.full(
            [1, 64, 3, 3],
            1e-3,
            dtype=torch.float,
            device=cpu_device,
            requires_grad=True,
        )
        ref = conv(x)
        ref.backward(grad)
        y_cpu_gw = conv.weight.grad.detach().clone()

        conv.zero_grad()

        with torch.xpu.onednn_layout():
            conv_xpu = conv.to(dpcpp_device)
            x_xpu = x.to(dpcpp_device)
            grad_xpu = grad.to(dpcpp_device)
            real = conv_xpu(x_xpu)
            real.backward(grad_xpu)
            y_dpcpp_gw = conv_xpu.weight.grad.detach().clone()

            self.assertEqual(real.cpu(), ref)
            self.assertEqual(y_dpcpp_gw.cpu(), y_cpu_gw)

    def test_group_conv_channels_last(self, dtype=torch.float):
        conv = nn.Conv2d(
            256, 64, kernel_size=3, stride=1, padding=1, bias=False, groups=2
        ).to(cpu_device)
        x = torch.randn(
            [1, 256, 3, 3], dtype=torch.float, device=cpu_device, requires_grad=True
        ).to(cpu_device)
        grad = torch.full(
            [1, 64, 3, 3],
            1e-3,
            dtype=torch.float,
            device=cpu_device,
            requires_grad=True,
        )
        ref = conv(x)
        ref.backward(grad)
        y_cpu_gw = conv.weight.grad.detach().clone()

        conv.zero_grad()

        conv_xpu = conv.to(dpcpp_device).to(memory_format=torch.channels_last)
        x_xpu = x.to(dpcpp_device).to(memory_format=torch.channels_last)
        grad_xpu = grad.to(dpcpp_device).to(memory_format=torch.channels_last)
        real = conv_xpu(x_xpu)
        real.backward(grad_xpu)
        y_dpcpp_gw = conv_xpu.weight.grad.detach().clone()

        self.assertEqual(real.cpu(), ref)
        self.assertEqual(y_dpcpp_gw.cpu(), y_cpu_gw)
        self.assertTrue(real.is_contiguous(memory_format=torch.channels_last))
        self.assertTrue(y_dpcpp_gw.is_contiguous(memory_format=torch.channels_last))

    def test_group_conv3d(self, dtype=torch.float):
        conv = nn.Conv3d(
            256, 64, kernel_size=3, stride=1, padding=1, bias=False, groups=2
        ).to(dpcpp_device)
        x = torch.randn(
            [1, 256, 3, 3, 3], dtype=torch.float, device=cpu_device, requires_grad=True
        ).to(dpcpp_device)
        grad = torch.full(
            [1, 64, 3, 3, 3],
            1e-3,
            dtype=torch.float,
            device=cpu_device,
            requires_grad=True,
        ).to(dpcpp_device)
        real = conv(x)
        real.backward(grad)
        y_dpcpp_gw = conv.weight.grad.detach().clone()

        conv.zero_grad()

        conv_cpu = conv.cpu()
        x_cpu = x.cpu()
        grad_cpu = grad.cpu()
        ref = conv_cpu(x_cpu)
        ref.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        self.assertEqual(real.cpu(), ref)
        self.assertEqual(y_dpcpp_gw.cpu(), y_cpu_gw)

    def test_group_conv3d_blk(self, dtype=torch.float):
        conv = nn.Conv3d(
            256, 64, kernel_size=3, stride=1, padding=1, bias=False, groups=2
        ).to(cpu_device)
        x = torch.randn(
            [1, 256, 3, 3, 3], dtype=torch.float, device=cpu_device, requires_grad=True
        ).to(cpu_device)
        grad = torch.full(
            [1, 64, 3, 3, 3],
            1e-3,
            dtype=torch.float,
            device=cpu_device,
            requires_grad=True,
        )
        ref = conv(x)
        ref.backward(grad)
        y_cpu_gw = conv.weight.grad.detach().clone()

        conv.zero_grad()

        with torch.xpu.onednn_layout():
            conv_xpu = conv.to(dpcpp_device)
            x_xpu = x.to(dpcpp_device)
            grad_xpu = grad.to(dpcpp_device)
            real = conv_xpu(x_xpu)
            real.backward(grad_xpu)
            y_dpcpp_gw = conv_xpu.weight.grad.detach().clone()

            self.assertEqual(real.cpu(), ref)
            self.assertEqual(y_dpcpp_gw.cpu(), y_cpu_gw)

    def test_group_conv3d_channels_last(self, dtype=torch.float):
        conv = nn.Conv3d(
            256, 64, kernel_size=3, stride=1, padding=1, bias=False, groups=2
        ).to(cpu_device)
        x = torch.randn(
            [1, 256, 3, 3, 3], dtype=torch.float, device=cpu_device, requires_grad=True
        ).to(cpu_device)
        grad = torch.full(
            [1, 64, 3, 3, 3],
            1e-3,
            dtype=torch.float,
            device=cpu_device,
            requires_grad=True,
        )
        ref = conv(x)
        ref.backward(grad)
        y_cpu_gw = conv.weight.grad.detach().clone()

        conv.zero_grad()

        conv_xpu = conv.to(dpcpp_device).to(memory_format=torch.channels_last_3d)
        x_xpu = x.to(dpcpp_device).to(memory_format=torch.channels_last_3d)
        grad_xpu = grad.to(dpcpp_device).to(memory_format=torch.channels_last_3d)
        real = conv_xpu(x_xpu)
        real.backward(grad_xpu)
        y_dpcpp_gw = conv_xpu.weight.grad.detach().clone()

        self.assertEqual(real.cpu(), ref)
        self.assertEqual(y_dpcpp_gw.cpu(), y_cpu_gw)
        self.assertTrue(real.is_contiguous(memory_format=torch.channels_last_3d))
        self.assertTrue(y_dpcpp_gw.is_contiguous(memory_format=torch.channels_last_3d))

    def test_plain_format_bwd(self, dtype=torch.float):
        x_cpu = torch.randn(
            [1, 7, 1, 15000], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [1, 64, 1, 15000], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv2d(7, 64, kernel_size=1, stride=1, bias=True)
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()
        conv_cpu.zero_grad()

        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_()
        grad_dpcpp = grad_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()
        conv_cpu.zero_grad()

        self.assertEqual(y_cpu, y_dpcpp.cpu(), atol=5 * 1e-5, rtol=0)
        self.assertEqual(y_cpu_gw, y_dpcpp_gw.cpu(), atol=5 * 1e-5, rtol=0)

    def test_conv2d_corner_case(self, dtype=torch.float):
        conv_cpu = nn.Conv2d(
            3, 32, kernel_size=(3, 3), stride=(2, 2), padding=(1, 1), bias=False
        )

        x_cpu = torch.randn([1, 3, 300, 300], dtype=dtype, device=cpu_device)
        x_cpu = torch.as_strided(x_cpu, x_cpu.shape, (3, 1, 900, 3))
        y_cpu = conv_cpu(x_cpu)

        x_dpcpp = x_cpu.to(dpcpp_device)
        conv_dpcpp = conv_cpu.to(dpcpp_device)
        y_dpcpp = conv_dpcpp(x_dpcpp)

        self.assertEqual(y_cpu, y_dpcpp.cpu())

    def test_conv2d_bia_bf16_input_bf16_bia(self, dtype=torch.float):
        x_cpu = torch.randn(
            [1, 64, 256, 256], dtype=dtype, device=cpu_device, requires_grad=True
        )
        grad_cpu = torch.full(
            [1, 64, 256, 256], 1e-3, dtype=dtype, device=cpu_device, requires_grad=True
        )
        conv_cpu = nn.Conv2d(64, 64, kernel_size=3, stride=1, padding=1, bias=True)
        y_cpu = conv_cpu(x_cpu)
        y_cpu.backward(grad_cpu)
        y_cpu_gw = conv_cpu.weight.grad.detach().clone()

        conv_cpu.zero_grad()

        dtype_dpcpp = torch.bfloat16
        x_dpcpp = x_cpu.to(dpcpp_device).requires_grad_().to(dtype_dpcpp)
        grad_dpcpp = grad_cpu.to(dpcpp_device).to(dtype_dpcpp)
        conv_dpcpp = conv_cpu.to(dpcpp_device).to(dtype_dpcpp)
        y_dpcpp = conv_dpcpp(x_dpcpp)
        y_dpcpp.backward(grad_dpcpp)
        y_dpcpp_gw = conv_dpcpp.weight.grad.detach().clone()

        self.assertEqual(y_cpu, y_dpcpp.to(torch.float).cpu(), rtol=10e-4, atol=10e-2)
        self.assertEqual(
            y_cpu_gw, y_dpcpp_gw.to(torch.float).cpu(), rtol=10e-4, atol=10e-2
        )
