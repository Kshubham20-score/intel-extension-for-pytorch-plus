import torch
from torch.testing._internal.common_utils import TestCase
import intel_extension_for_pytorch as ipex  # noqa
ipex.compatible_mode()
from unittest import skip

cuda_device = torch.device("cuda")


class TestTorchMethod(TestCase):
    def test_is_available(self):
        torch.cuda.is_available()

    def test_init(self):
        torch.cuda.init()
        self.assertEqual(True, torch.cuda.is_initialized())

    def test_device_count(self):
        x = torch.cuda.device_count()
        y = torch.xpu.device_count()
        self.assertEqual(x, y)

    def test_get_device_name(self):
        device_id = torch.xpu.current_device()
        x = torch.xpu.get_device_name(device_id)
        device_id = torch.cuda.current_device()
        y = torch.cuda.get_device_name(device_id)
        self.assertEqual(x, y)

    def test_current_device(self):
        x = torch.xpu.current_device()
        y = torch.cuda.current_device()
        self.assertEqual(x, y)

    def test_set_device(self):
        device_id = torch.cuda.current_device()
        torch.cuda.set_device(device_id)

    @skip("Known issue for torch.xpu.FloatTensor")
    def test_float_tensor(self):
        x = torch.cuda.FloatTensor(224, 224, 32, device=torch.device("cuda"))
        self.assertEqual(x.device.type, "xpu")

    @skip("Known issue for torch.xpu.IntTensor")
    def test_int_tensor(self):
        x = torch.cuda.IntTensor([3, 4])
        self.assertEqual(x.device.type, "xpu")

    def test_empty_cache(self):
        torch.cuda.empty_cache()

    def test_device(self):
        cuda_device = torch.device("cuda")
        self.assertEqual(cuda_device.type, "xpu")

        cuda_num_device = torch.device("cuda:0")
        self.assertEqual(cuda_num_device.type, "xpu")

        num_device = torch.device(0)
        self.assertEqual(num_device.type, "xpu")

        cpu_device = torch.device("cpu")
        self.assertEqual(cpu_device.type, "cpu")

        cpu_num_device = torch.device("cpu:0")
        self.assertEqual(cpu_num_device.type, "cpu")

        meta_device = torch.device("meta")
        self.assertEqual(meta_device.type, "meta")

        xpu_device = torch.device("xpu")
        self.assertEqual(xpu_device.type, "xpu")

        xpu_num_device = torch.device("xpu:0")
        self.assertEqual(xpu_num_device.type, "xpu")

    def test_index_device(self):
        torch.cuda.set_device(0)
        device = torch.device("cuda")
        self.assertEqual(device.type, "xpu")
