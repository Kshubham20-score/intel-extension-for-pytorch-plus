import torch
from torch.testing._internal.common_utils import TestCase

import intel_extension_for_pytorch  # noqa


class TestTorchMethod(TestCase):
    def test_logical_and(self, dtype=torch.float):
        input1 = torch.tensor([0, 1, 10, 0], dtype=torch.int8)
        input2 = torch.tensor([4, 0, 1, 0], dtype=torch.int8)

        # TODO: check for diferent dtype
        array1 = [input1, input1.half(), input1.bool()]
        array2 = [input2, input2.half(), input2.bool()]
        if torch.xpu.has_fp64_dtype():
            array1.append(input1.double())
            array2.append(input2.double())

        for input1, input2 in zip(array1, array2):
            # print("Testing logical_and on", input1, "and", input2)
            input1_dpcpp = input1.to("xpu")
            input2_dpcpp = input2.to("xpu")
            # print("SYCL result:")
            # print("--torch.logical_and--")
            result_cpu1 = torch.logical_and(input1, input2)
            result_dpcpp1 = torch.logical_and(input1_dpcpp, input2_dpcpp)
            # print(result_dpcpp1.to("cpu"))

            # print("--tensor.logical_and--")
            result_cpu2 = input1.logical_and(input2)
            result_dpcpp2 = input1_dpcpp.logical_and(input2_dpcpp)
            # print(result_dpcpp2.to("cpu"))
            # print("--tensor.logical_and_--")
            result_cpu3 = input2.logical_and_(input1)
            result_dpcpp3 = input2_dpcpp.logical_and_(input1_dpcpp)
            # print(result_dpcpp3.to("cpu"))
            # print("\n")
            self.assertEqual(result_cpu1, result_dpcpp1.cpu())
            self.assertEqual(result_cpu2, result_dpcpp2.cpu())
            self.assertEqual(result_cpu3, result_dpcpp3.cpu())

        # print("Additional Test with out=torch.empty(4, dtype=torch.int8)")
        # print("on", input1, input2)
        out = torch.empty(4, dtype=torch.bool)
        result = torch.logical_and(input1, input2, out=out)
        # print("CPU result:")
        # print(result)
        # print("SYCL result:")
        result_dpcpp = torch.logical_and(
            input1.to("xpu"), input2.to("xpu"), out=out.to("xpu")
        )
        # print(result_dpcpp.to("cpu"))
        self.assertEqual(result, result_dpcpp)

    def test_logical_or(self, dtype=torch.float):
        input1 = torch.tensor([0, 1, 10, 0], dtype=torch.int8)
        input2 = torch.tensor([4, 0, 1, 0], dtype=torch.int8)

        # TODO: check for diferent dtype
        array1 = [input1, input1.half(), input1.bool()]
        array2 = [input2, input2.half(), input2.bool()]
        if torch.xpu.has_fp64_dtype():
            array1.append(input1.double())
            array2.append(input2.double())

        for input1, input2 in zip(array1, array2):
            # print("Testing logical_or on", input1, "and", input2)
            input1_dpcpp = input1.to("xpu")
            input2_dpcpp = input2.to("xpu")
            # print("SYCL result:")
            # print("--torch.logical_or--")
            result_cpu1 = torch.logical_or(input1, input2)
            result_dpcpp1 = torch.logical_or(input1_dpcpp, input2_dpcpp)
            # print(result_dpcpp1.to("cpu"))

            # print("--tensor.logical_or--")
            result_cpu2 = input1.logical_or(input2)
            result_dpcpp2 = input1_dpcpp.logical_or(input2_dpcpp)
            # print(result_dpcpp2.to("cpu"))
            # print("--tensor.logical_or_--")
            result_cpu3 = input2.logical_or_(input1)
            result_dpcpp3 = input2_dpcpp.logical_or_(input1_dpcpp)
            # print(result_dpcpp3.to("cpu"))
            # print("\n")
            self.assertEqual(result_cpu1, result_dpcpp1.cpu())
            self.assertEqual(result_cpu2, result_dpcpp2.cpu())
            self.assertEqual(result_cpu3, result_dpcpp3.cpu())

        # print("Additional Test with out=torch.empty(4, dtype=torch.int8)")
        # print("on", input1, input2)
        out = torch.empty(4, dtype=torch.bool)
        result = torch.logical_or(input1, input2, out=out)
        # print("CPU result:")
        # print(result)
        # print("SYCL result:")
        result_dpcpp = torch.logical_or(
            input1.to("xpu"), input2.to("xpu"), out=out.to("xpu")
        )
        # print(result_dpcpp.to("cpu"))
        self.assertEqual(result, result_dpcpp)

    def test_logical_xor(self, dtype=torch.float):
        input1 = torch.tensor([0, 1, 10, 0], dtype=torch.int8)
        input2 = torch.tensor([4, 0, 1, 0], dtype=torch.int8)

        # TODO: check for diferent dtype
        array1 = [input1, input1.half(), input1.bool()]
        array2 = [input2, input2.half(), input2.bool()]
        if torch.xpu.has_fp64_dtype():
            array1.append(input1.double())
            array2.append(input2.double())

        for input1, input2 in zip(array1, array2):
            # print("Testing logical_xor on", input1, "and", input2)
            input1_dpcpp = input1.to("xpu")
            input2_dpcpp = input2.to("xpu")
            # print("SYCL result:")
            # print("--torch.logical_xor--")
            result_cpu1 = torch.logical_xor(input1, input2)
            result_dpcpp1 = torch.logical_xor(input1_dpcpp, input2_dpcpp)
            # print(result_dpcpp1.to("cpu"))

            # print("--tensor.logical_xor--")
            result_cpu2 = input1.logical_xor(input2)
            result_dpcpp2 = input1_dpcpp.logical_xor(input2_dpcpp)
            # print(result_dpcpp2.to("cpu"))
            # print("--tensor.logical_xor_--")
            result_cpu3 = input2.logical_xor_(input1)
            result_dpcpp3 = input2_dpcpp.logical_xor_(input1_dpcpp)
            # print(result_dpcpp3.to("cpu"))
            # print("\n")
            self.assertEqual(result_cpu1, result_dpcpp1.cpu())
            self.assertEqual(result_cpu2, result_dpcpp2.cpu())
            self.assertEqual(result_cpu3, result_dpcpp3.cpu())

        # print("Additional Test with out=torch.empty(4, dtype=torch.int8)")
        # print("on", input1, input2)
        out = torch.empty(4, dtype=torch.bool)
        result = torch.logical_xor(input1, input2, out=out)
        # print("CPU result:")
        # print(result)
        # print("SYCL result:")
        result_dpcpp = torch.logical_xor(
            input1.to("xpu"), input2.to("xpu"), out=out.to("xpu")
        )
        # print(result_dpcpp.to("cpu"))
        self.assertEqual(result, result_dpcpp)
