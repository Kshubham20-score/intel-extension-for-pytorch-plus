import torch
from torch.testing._internal.common_utils import TestCase
import intel_extension_for_pytorch  # noqa

import numpy as np

np.set_printoptions(threshold=np.inf)

cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestTorchMethod(TestCase):
    def test_index_select(self, dtype=torch.float):
        dim_size = 10
        dims = 3

        def _test_index_select(input, indcies):
            def _test(input, indcies, dim):
                y_cpu = torch.index_select(input, dim, indices)
                y_dpcpp = torch.index_select(
                    input.to(dpcpp_device), dim, indcies.to(dpcpp_device)
                )
                print(y_dpcpp.size())
                print(y_dpcpp.cpu())
                self.assertEqual(y_cpu, y_dpcpp.cpu())

            _test(input, indcies, 0)
            _test(input, indcies, 1)
            _test(input, indcies, 2)
            _test(input, indcies, 3)

        # x = torch.linspace(0, dim_size ** dims - 1, steps=dim_size ** dims, dtype=torch.double,
        #                    device=dpcpp_device).view([dim_size for d in range(dims)])

        x = torch.linspace(0, 6 * 7 * 8 * 9 - 1, steps=6 * 7 * 8 * 9).view(6, 7, 8, 9)
        indices = torch.LongTensor([0, 2])

        _test_index_select(x, indices)

        # input transpose

        _test_index_select(torch.transpose(x, 0, 1), indices)

        _test_index_select(torch.transpose(x, 0, 2), indices)

        _test_index_select(torch.transpose(x, 0, 3), indices)

        _test_index_select(torch.transpose(x, 1, 2), indices)

        _test_index_select(torch.transpose(x, 1, 3), indices)

        _test_index_select(torch.transpose(x, 2, 3), indices)

    def test_index_select_out_non_contiguous(self, dtype=torch.float):
        # Transformer case
        src_xpu = torch.rand((400 * 202,), device=torch.device("xpu"))
        src_xpu = src_xpu.as_strided((400, 1), (202, 1))
        src_cpu = src_xpu.cpu()
        dst_xpu = torch.rand((400 * 202,), device=torch.device("xpu"))
        dst_xpu = dst_xpu.as_strided((400, 1), (202, 1))
        dst_cpu = dst_xpu.cpu()
        idx_xpu = torch.randint(
            0, 400, (400,), dtype=torch.long, device=torch.device("xpu")
        )
        idx_cpu = idx_xpu.cpu()
        torch.index_select(src_cpu, dim=0, index=idx_cpu, out=dst_cpu)
        torch.index_select(src_xpu, dim=0, index=idx_xpu, out=dst_xpu)
        self.assertEqual(dst_cpu, dst_xpu.cpu())

    def test_index_select_out_single_batch(self, dtype=torch.float):
        # Transformer case
        src_xpu = torch.rand(1, 333, dtype=torch.float, device=torch.device("xpu"))
        src_cpu = src_xpu.cpu()
        idx_xpu = torch.tensor(
            (0, 0, 0, 0), dtype=torch.long, device=torch.device("xpu")
        )
        idx_cpu = idx_xpu.cpu()
        dst_xpu = src_xpu.index_select(0, idx_xpu)
        dst_cpu = src_cpu.index_select(0, idx_cpu)
        self.assertEqual(dst_cpu, dst_xpu.cpu())


# indcies transposed
# test_index_select(x, torch.transpose(indices, 0, 1))


# # extra word embedding test
#
# print("extra word embedding test")
# print("cpu")
# # an Embedding module containing 10 tensors of size 3
# embedding = nn.Embedding(30522, 765)
# print(embedding.weight)
#
# # a batch of 2 samples of 4 indices each
# input = torch.LongTensor([101])
# res = embedding(input)
#
# print(res[0, 0:10])
#
# print("xpu")
# embedding.dpcpp()
#
# res = embedding(input.to(dpcpp_device))
# print(res.cpu()[0, 0:10])
#
# # test index select on bool tensor
# x = torch.randn([6, 7, 8, 9], dtype=torch.float, device=dpcpp_device)
# x = x.gt(0)
#
# test_index_select(x, indices)
