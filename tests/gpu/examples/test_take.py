import torch
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa
import pytest

cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestNNMethod(TestCase):
    @pytest.mark.skip(
        reason="PT2.5: Total number of work-items in a work-group cannot exceed 512 for this kernel \
            -54 (PI_ERROR_INVALID_WORK_GROUP_SIZE)"
    )
    def test_take(self, dtype=torch.float):
        src = torch.rand(2, 3)
        print(src)

        dst = torch.take(src, torch.tensor([0, 2, 5]))
        print("dst = ", dst)

        src_dpcpp = src.to("xpu")
        idx_dpcpp = torch.tensor(
            [0, 2, 5], device=torch.device("xpu"), dtype=torch.long
        )
        print(idx_dpcpp.shape)
        dst_dpcpp_1 = torch.take(src_dpcpp, idx_dpcpp)
        # dst_dpcpp_2 = torch.take(dst_dpcpp_1, torch.tensor([0], device=torch.device("xpu"), dtype=torch.long))
        print("dst_dpcpp_1 = ", dst_dpcpp_1.cpu())
        # print("dst_dpcpp_2 = ", dst_dpcpp_2.cpu())
        self.assertEqual(src, src_dpcpp.cpu())
        self.assertEqual(dst, dst_dpcpp_1.cpu())
