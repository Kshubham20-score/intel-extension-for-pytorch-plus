import contextlib
import torch
import intel_extension_for_pytorch as ipex
import time
import json
import pathlib
import argparse
import os
import sys
from torch.nn.functional import pad
from datasets import load_dataset
from torch.utils.data import DataLoader

from transformers import (
    AutoConfig,
    AutoTokenizer,
    LlamaTokenizer,
)
from intel_extension_for_transformers.transformers.modeling import AutoModelForCausalLM
from intel_extension_for_transformers.transformers import RtnConfig
from intel_extension_for_transformers.transformers.llm.utils.generation import _beam_search, _greedy_search

# supported models
MODEL_CLASSES = {
    "auto": (AutoModelForCausalLM, AutoTokenizer),
    "gpt-j": (AutoModelForCausalLM, AutoTokenizer),
    "llama": (AutoModelForCausalLM, AutoTokenizer),
    "qwen": (AutoModelForCausalLM, AutoTokenizer),
    "phi-3": (AutoModelForCausalLM, AutoTokenizer),
}

# args
parser = argparse.ArgumentParser("Generation script (fp32/bf16 path)", add_help=False)
parser.add_argument(
    "-m",
    "--model-id",
    type=str,
    default="EleutherAI/gpt-j-6B",
    help="the huggingface mdoel id",
)
parser.add_argument('--sub-model-name',
    type=str,
    help="the sub model name for accuracy check"
)
parser.add_argument(
    "--device",
    type=str,
    choices=["cpu", "xpu"],
    default="cpu",
    help="cpu",
)
parser.add_argument(
    "--dtype",
    type=str,
    choices=["float32", "bfloat16", "float16"],
    default="bfloat16",
    help="float16, bfloat16, float32",
)
parser.add_argument(
    "--input-tokens",
    default="32",
    type=str,
    help="input tokens length if needed from prompt.json",
)
parser.add_argument(
    "--max-new-tokens", default=32, type=int, help="output max new tokens"
)
parser.add_argument(
    "--prompt", default=None, type=str, help="input prompt for self-defined if needed"
)
parser.add_argument("--greedy", action="store_true")
parser.add_argument("--ipex", action="store_true")
parser.add_argument("--jit", action="store_true")
parser.add_argument("--profile", action="store_true")
parser.add_argument("--benchmark", action="store_true")
parser.add_argument("--lambada", action="store_true")
parser.add_argument("--dataset", default="lambada", type=str)
parser.add_argument("--num-beams", default=4, type=int, help="beam width")
parser.add_argument("--num-iter", default=10, type=int, help="num iter")
parser.add_argument("--num-warmup", default=3, type=int, help="num warmup")
parser.add_argument("--batch-size", default=1, type=int, help="batch size")
parser.add_argument(
    "--token-latency", action="store_true", help="get token latency breakdown"
)
parser.add_argument("--print-memory", action="store_true")
parser.add_argument("--disable_optimize_transformers", action="store_true")
# WOQ related args.
parser.add_argument("--woq", action="store_true")
parser.add_argument("--calib_dataset", default="wikitext2", type=str)
parser.add_argument("--calib_group_size", default=-1, type=int)
parser.add_argument("--calib_output_dir", default="./", type=str)
parser.add_argument("--calib_checkpoint_name", default="quantized_weight.pt", type=str)
parser.add_argument("--calib_nsamples", default=128, type=int)
parser.add_argument("--calib_wbits", default=4, type=int)
parser.add_argument("--calib_seed", default=0, type=int)
parser.add_argument("--woq_checkpoint_path", default="", type=str)
parser.add_argument("--woq_algo", default="RTN", choices=["RTN"], help="WOQ algorithm to apply")
parser.add_argument("--save_model", action="store_true")
parser.add_argument("--output_dir", type=str, default="./", help="the dir to save quantized model")
parser.add_argument("--accuracy-only", action="store_true")
parser.add_argument(
    "--acc-tasks",
    default="lambada_standard",
    type=str,
    help="tasks list for accuracy validation, only enabled lambada_standard and lambada_standard at present",
)
parser.add_argument("--acc-iter", default=-1, type=int)
args = parser.parse_args()
print(args)

do_profiling = os.environ.get("PROFILE", "OFF").upper() in ["1", "Y", "ON", "YES", "TRUE"]
do_profiling = args.profile or do_profiling

# device
device = torch.device(args.device)

def get_memory_usage(name, args):
    if args.print_memory:
        if args.device == "xpu":
            memory_allocated = round(torch.xpu.memory_reserved() / 1024**3, 3)
        print(name, "memory used total:", memory_allocated, "GB")

# dtype
amp_enabled = True if args.dtype != "float32" else False
amp_dtype = getattr(torch, args.dtype)

# load model
model_type = next(
    (x for x in MODEL_CLASSES.keys() if x in args.model_id.lower()), "auto"
)
model_class = MODEL_CLASSES[model_type]
if args.woq_checkpoint_path:
    tokenizer = AutoTokenizer.from_pretrained(args.woq_checkpoint_path, trust_remote_code=True)
    config = AutoConfig.from_pretrained(args.woq_checkpoint_path, use_cache=True, # to use kv cache.
                                        trust_remote_code=True)
else:
    config = AutoConfig.from_pretrained(args.model_id, torchscript=args.jit, trust_remote_code=True)
    tokenizer = model_class[1].from_pretrained(args.model_id, trust_remote_code=True)
if not hasattr(config, "text_max_length") and args.prompt is None:
    config.text_max_length = int(args.input_tokens) + int(args.max_new_tokens)


if args.woq_checkpoint_path:
    # directly load already quantized model
    model = AutoModelForCausalLM.from_pretrained(
        args.woq_checkpoint_path, trust_remote_code=True, device_map="xpu", torch_dtype=torch.float16)
    model = model.to(memory_format=torch.channels_last)
    woq_quantization_config = getattr(model, "quantization_config", None)
else:
    # do quantization 
    if args.woq_algo == "RTN":
        woq_quantization_config = RtnConfig(compute_dtype="fp16", weight_dtype="int4_fullrange", scale_dtype="fp16", group_size=64)
    else:
        print(f"unsupported woq algorithm: {args.woq_algo}")
        sys.exit(0)
    model = model_class[0].from_pretrained(
        args.model_id,
        device_map=device,
        quantization_config=woq_quantization_config,
        trust_remote_code=True,
        use_llm_runtime=False
    )
    if args.save_model:
        model.save_pretrained(args.output_dir)
        tokenizer.save_pretrained(args.output_dir)

model = model.eval().to(device)
model = model.to(memory_format=torch.channels_last)

print(model)
# to ipex
model = ipex.optimize_transformers(model.eval(), device="xpu", inplace=True, quantization_config=woq_quantization_config)
get_memory_usage("Ipex", args)


num_beams = 1 if args.greedy else args.num_beams
# generate args
generate_kwargs = dict(do_sample=False, temperature=0.9, num_beams=num_beams)


######################## run lm eval accuracy check ########################
def run_accuracy():
    from lm_eval import evaluator
    from lm_eval.models.huggingface import HFLM
    from lm_eval.utils import make_table

    os.environ["TOKENIZERS_PARALLELISM"] = "false"

    hfmodel = HFLM(
        pretrained=model,
        tokenizer=tokenizer,
        batch_size=args.batch_size,
        device=args.device,
    )

    if args.acc_iter == -1:
        results = evaluator.simple_evaluate(
            model=hfmodel,
            tasks=args.acc_tasks,
        )
    else:
        results = evaluator.simple_evaluate(
            model=hfmodel,
            tasks=args.acc_tasks,
            limit=args.acc_iter
        )

    print(make_table(results))


if args.accuracy_only:
    run_accuracy()
    sys.exit(0)

######################## run generation benchmark ########################
current_path = pathlib.Path(__file__).parent.resolve()
with open(str(current_path) + "/prompt.json", encoding="utf8") as f:
    prompt_pool = json.load(f)

def run_generate(num_tokens, num_input_tokens, num_beams):
    print(f"*** Starting to generate {num_tokens} tokens for {num_input_tokens} tokens with num_beams={num_beams}")
    if args.prompt is not None:
        prompt = args.prompt
    elif model_type == "auto":
        raise SystemExit(
            "[ERROR] model prompt is not supported, please use --prompt for this model: "
            + args.model_id
        )
    elif int(args.input_tokens) > 8192:
        prompt = prompt_pool[model_type]["8192"] * int(int(args.input_tokens) / 8192)
    elif args.input_tokens in prompt_pool[model_type]:
        prompt = prompt_pool[model_type][args.input_tokens]
    else:
        raise SystemExit("[ERROR] Plese use --prompt if want to use custom input.")

    input_size = tokenizer(prompt, return_tensors="pt").input_ids.size(dim=1)
    print("---- Prompt size:", input_size)

    if args.token_latency:
        generate_kwargs["token_latency"] = True

    # Accuracy check, take the ref_prompt as reference for check
    f1 = open(os.path.join(os.path.dirname(__file__), "ref_prompt.json"), encoding="utf8")
    prompt_json = json.load(f1)
    f1.close()
    ref_prompt=None
    ref_prompt_cuda=None
    token_support = [(32, 32), (1024, 128)]
    if (int(num_input_tokens), num_tokens) in token_support:
        ref_prompt = prompt_json[args.sub_model_name][f"{num_input_tokens}-{num_tokens}"][f"{num_beams}"]
        try:
            ref_prompt_cuda = prompt_json[args.sub_model_name][f"{num_input_tokens}-{num_tokens}"][f"cuda-result: {num_beams}"]
        except Exception:
            pass
    acc_pass = 0

    # start
    total_time = 0.0
    num_iter = args.num_iter
    num_warmup = args.num_warmup
    prompt = [prompt] * args.batch_size
    if args.token_latency:
        ipex.transformers.optimize.convert_function(model, "_greedy_search", _greedy_search)
        if args.disable_optimize_transformers:
            ipex.transformers.optimize.convert_function(model, "_beam_search", _beam_search)
        model.config.token_latency = True
    total_list = []
    with torch.inference_mode(), torch.no_grad(), torch.autocast(
        device_type=args.device,
        enabled=amp_enabled,
        dtype=amp_dtype if amp_enabled else None,
    ):
        for i in range(num_iter):
            tic = time.time()
            with (
                contextlib.nullcontext() if not do_profiling else
                torch.profiler.profile(
                    activities=[torch.profiler.ProfilerActivity.CPU,
                                torch.profiler.ProfilerActivity.XPU],
                    record_shapes=True,
                )
            ) as prof:
                input_ids = tokenizer(prompt, return_tensors="pt").input_ids.to(device)
                output = model.generate(
                    input_ids, max_new_tokens=int(args.max_new_tokens), **generate_kwargs
                )
                gen_ids = output[0] if args.token_latency else output
                gen_text = tokenizer.batch_decode(gen_ids, skip_special_tokens=True)
                if args.device == "xpu":
                    torch.xpu.synchronize()
            if do_profiling:
                torch.save(prof.key_averages().table(sort_by="self_xpu_time_total"), "./profile.pt")
                # Cannot sort by id when using kineto
                # torch.save(prof.table(sort_by="id", row_limit=-1),'./profile_id.pt')
                torch.save(prof.key_averages(group_by_input_shape=True).table(), "./profile_detail.pt")
                prof.export_chrome_trace("./trace.json")
            toc = time.time()
            print("")
            input_tokens_lengths = [x.shape[0] for x in input_ids]
            output_tokens_lengths = [x.shape[0] for x in gen_ids]
            total_new_tokens = [
                o - i if model.config.model_type != "t5" else o
                for i, o in zip(input_tokens_lengths, output_tokens_lengths)
            ]
            print(gen_text, total_new_tokens, flush=True)
            print("Iteration: %d, Time: %.6f sec" % (i, toc - tic), flush=True)
            if i >= num_warmup:
                total_time += toc - tic
                if args.token_latency:
                    total_list.append(output[1])
            if ref_prompt is not None and ref_prompt in gen_text:
                acc_pass += 1
            elif ref_prompt_cuda is not None and ref_prompt_cuda in gen_text:
                acc_pass += 1

    print("\n", "-" * 10, "Summary:", "-" * 10)
    latency = total_time / (num_iter - num_warmup)
    print("Inference latency: %.3f sec." % latency)

    if args.token_latency:
        import numpy as np
        from itertools import chain

        first_latency = np.mean([x[0] for x in total_list])
        average_2n = list(chain(*[x[1:] for x in total_list]))
        average_2n.sort()
        average_2n_latency = np.mean(average_2n)
        #p90_latency = average_2n[int(len(average_2n) * 0.9)]
        #p99_latency = average_2n[int(len(average_2n) * 0.99)]
        print("First token average latency: %.6f sec." % first_latency)
        print("Average 2... latency: %.6f sec." % average_2n_latency)
        #print("P90 2... latency: %.3f sec." % p90_latency)
        #print("P99 2... latency: %.3f sec." % p99_latency)

    if ref_prompt is None:
        print("Accuracy check skip")
    elif acc_pass==args.num_iter:
        print("Accuracy check pass")
    else:
        print("Accuracy check fail, the wrong iteration number is:", args.num_iter - acc_pass)

def to_list(obj):
    if not isinstance(obj, list):
        return [obj]
    else:
        return obj

for o, i, g in zip(to_list(args.max_new_tokens), to_list(args.input_tokens), to_list(args.num_beams)):
    run_generate(o, i, g)
