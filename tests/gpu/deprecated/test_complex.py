import torch
from torch.testing._internal.common_utils import TestCase
import intel_extension_for_pytorch  # noqa
import pytest


class TestTorchMethod(TestCase):
    def test_complex_float(self, dtype=torch.float):
        img = torch.randn([5, 5])
        real = torch.randn([5, 5])
        y_cpu = torch.complex(real, img)
        img_xpu = img.to("xpu")
        real_xpu = real.to("xpu")
        y_xpu = torch.complex(real_xpu, img_xpu)
        self.assertEqual(y_cpu, y_xpu.to("cpu"))

    @pytest.mark.skipif(
        not torch.xpu.has_fp64_dtype(), reason="fp64 not support by this device"
    )
    def test_complex_double(self, dtype=torch.double):
        img = torch.randn([5, 5], dtype=dtype)
        real = torch.randn([5, 5], dtype=dtype)
        y_cpu = torch.complex(real, img)
        img_xpu = img.to("xpu")
        real_xpu = real.to("xpu")
        y_xpu = torch.complex(real_xpu, img_xpu)
        self.assertEqual(y_cpu, y_xpu.to("cpu"))

    def test_real_imag(self, dtype=torch.float):
        input = torch.tensor([-1 + 1j, -2 + 2j, 3 - 3j])
        input_xpu = input.to("xpu")
        self.assertEqual(input.real, input_xpu.real.to("cpu"))
        self.assertEqual(input.imag, input_xpu.imag.to("cpu"))

    def test_conj(self, dtype=torch.float):
        input = torch.tensor([-1 + 1j, -2 + 2j, 3 - 3j])
        input_xpu = input.to("xpu")
        self.assertEqual(input.conj(), input_xpu.conj().to("cpu"))

    def test_complex_norm(self, dtype=torch.cfloat):
        input = torch.tensor([0.9701 + 0.7078j, 0.345 - 1.764j])
        input_xpu = input.to("xpu")
        self.assertEqual(torch.norm(input), torch.norm(input_xpu).to("cpu"))

    def test_complex_copy(self, dtype=torch.chalf):
        input = torch.tensor([0.9701 + 0.7078j, 0.345 - 1.764j], dtype=dtype)
        print(input.type())
        output1 = input.to("xpu")
        output2 = output1.clone()
        self.assertEqual(input, output1.to("cpu"))
        self.assertEqual(input, output2.to("cpu"))
