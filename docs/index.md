# Intel® Extension for PyTorch\* Large Language Model (LLM) Feature Get Started For Llama 3.2 models

Intel® Extension for PyTorch\* provides dedicated optimization for running Llama 3.2 models faster, including technical points like paged attention, ROPE fusion, etc. And a set of data types are supported for various scenarios, including BF16, Weight Only Quantization INT4 (prototype), etc.

# 1. Environment Setup

There are several environment setup methodologies provided. You can choose either of them according to your usage scenario. The Docker-based ones are recommended.

## 1.1 [RECOMMENDED] Docker-based environment setup with pre-built wheels

```bash
# Get the Intel® Extension for PyTorch\* source code
git clone https://github.com/intel/intel-extension-for-pytorch.git
cd intel-extension-for-pytorch
git checkout 2.4-llama-3
git submodule sync
git submodule update --init --recursive

# Build an image with the provided Dockerfile by installing from Intel® Extension for PyTorch\* prebuilt wheel files
DOCKER_BUILDKIT=1 docker build -f examples/cpu/inference/python/llm/Dockerfile -t ipex-llm:2.4.0 .

# Run the container with command below
docker run --rm -it --privileged ipex-llm:2.4.0 bash

# When the command prompt shows inside the docker container, enter llm examples directory
cd llm

# Activate environment variables
source ./tools/env_activate.sh
```

## 1.2 Conda-based environment setup with pre-built wheels

```bash
# Get the Intel® Extension for PyTorch\* source code
git clone https://github.com/intel/intel-extension-for-pytorch.git
cd intel-extension-for-pytorch
git checkout 2.4-llama-3
git submodule sync
git submodule update --init --recursive

# Create a conda environment (pre-built wheel only available with python=3.10)
conda create -n llm python=3.10 -y
conda activate llm

# Setup the environment with the provided script
# A sample "prompt.json" file for benchmarking is also downloaded
cd examples/cpu/inference/python/llm
bash ./tools/env_setup.sh 7

# Activate environment variables
source ./tools/env_activate.sh
```
<br>

# 2. How To Run Llama 3.2 with ipex.llm

**ipex.llm provides a single script to facilitate running generation tasks as below:**

```
# if you are using a docker container built from commands above in Sec. 1.1, the placeholder LLM_DIR below is ~/llm
# if you are using a conda env created with commands above in Sec. 1.2, the placeholder LLM_DIR below is intel-extension-for-pytorch/examples/cpu/inference/python/llm
cd <LLM_DIR>
python run.py --help # for more detailed usages
```

| Key args of run.py | Notes |
|---|---|
| model id | "--model-name-or-path" or "-m" to specify the <LLAMA3_MODEL_ID_OR_LOCAL_PATH>, it is model id from Huggingface or downloaded local path |
| generation | default: beam search (beam size = 4), "--greedy" for greedy search |
| input tokens or prompt | provide fixed sizes for input prompt size, use "--input-tokens" for <INPUT_LENGTH> in [1024, 2048, 4096, 8192, 32768, 130944]; if "--input-tokens" is not used, use "--prompt" to choose other strings as inputs|
| input images | default: None, use "--image-url" to choose the image link address for vision-text tasks |
| vision text tasks | default: False, use "--vision-text-model" to choose if your model is running for vision-text generation tasks, default False meaning text generation tasks only|
| output tokens | default: 32, use "--max-new-tokens" to choose any other size |
| batch size |  default: 1, use "--batch-size" to choose any other size |
| token latency |  enable "--token-latency" to print out the first or next token latency |
| generation iterations |  use "--num-iter" and "--num-warmup" to control the repeated iterations of generation, default: 100-iter/10-warmup |
| streaming mode output | greedy search only (work with "--greedy"), use "--streaming" to enable the streaming generation output |

*Note:* You may need to log in your HuggingFace account to access the model files. Please refer to [HuggingFace login](https://huggingface.co/docs/huggingface_hub/quick-start#login).

## 2.1 Usage of running Llama 3.2 models

The _\<LLAMA3_MODEL_ID_OR_LOCAL_PATH\>_ in the below commands specifies the Llama 3.2 model you will run, which can be found from [HuggingFace Models](https://huggingface.co/models).

### 2.1.1 Run vision-text generation with Llama 3.2 11B models using BF16 autoTP (tensor parallel) on multiple CPU numa nodes

#### 2.1.1.1 Prepare:

```bash
unset KMP_AFFINITY
```

In the DeepSpeed cases below, we recommend "--shard-model" to shard model weight sizes more even for better memory usage when running with DeepSpeed.

If using "--shard-model", it will save a copy of the shard model weights file in the path of "--output-dir" (default path is "./saved_results" if not provided).
If you have used "--shard-model" and generated such a shard model path (or your model weights files are already well sharded), in further repeated benchmarks, please remove "--shard-model", and replace "-m <LLAMA3_MODEL_ID_OR_LOCAL_PATH>" with "-m <shard model path>" to skip the repeated shard steps.

Besides, the standalone shard model function/scripts are also provided in section 2.1.1.3, in case you would like to generate the shard model weights files in advance before running distributed inference.

#### 2.1.1.2 Commands:

- Command:
```bash
# Vision-Text generation inference
deepspeed --bind_cores_to_rank  run.py --benchmark -m <LLAMA3_MODEL_ID_OR_LOCAL_PATH> --dtype bfloat16 --ipex  --greedy --autotp --shard-model --vision-text-model  --prompt <PROMPT_TEXT>  --image-url <IMAGE_URL>

<PROMPT_TEXT> example:  "<|image|><|begin_of_text|>Describe all of the images briefly."
<IMAGE_URL> example "https://storage.googleapis.com/sfr-vision-language-research/BLIP/demo.jpg"
```

#### 2.1.1.3 How to Shard Model weight files for Distributed Inference with DeepSpeed

To save memory usage, we could shard the model weights files under the local path before we launch distributed tests with DeepSpeed.

```
cd ./utils
# general command:
python create_shard_model.py -m <LLAMA3_MODEL_ID_OR_LOCAL_PATH>  --save-path ./local_llama3_model_shard
# After sharding the model, using "-m ./local_llama3_model_shard" in later tests
```

### 2.1.2 Run text generation with Llama 3.2 3B models using Weight-only quantization (INT4) per CPU numa node

#### 2.1.2.1 Commands:
You can use auto-round (part of INC) to generate INT4 WOQ model with following steps.
- Environment installation:
```bash
pip install git+https://github.com/intel/auto-round.git@e24b9074af6cdb099e31c92eb81b7f5e9a4a244e
git clone https://github.com/intel/auto-round.git
git checkout e24b9074af6cdb099e31c92eb81b7f5e9a4a244e
cd auto-round/examples/language-modeling
```

- Command (quantize):
```bash
python3 main.py --model_name  $model_name --device cpu --sym --nsamples 512 --iters 1000 --group_size 32 --deployment_device cpu --disable_eval --output_dir <INT4_MODEL_SAVE_PATH>
```

- Command (benchmark):
```bash
cd <LLM_DIR>
# Text generation inference
OMP_NUM_THREADS=<physical cores num> numactl -m <node N> -C <physical cores list>  python run.py  --benchmark -m <LLAMA3_MODEL_ID_OR_LOCAL_PATH> --ipex-weight-only-quantization --weight-dtype INT4 --quant-with-amp --output-dir "saved_results"  --greedy --input-tokens <INPUT_LENGTH> --low-precision-checkpoint <INT4_MODEL_SAVE_PATH>

# Note that to get best throughput on multiple CPU numa nodes, we could further tune how many cores per instance and batch sizes (according to the latency requirement) to run multiple instances at the same time. 
```

#### 2.1.2.4 Notes:

(1) [_numactl_](https://linux.die.net/man/8/numactl) is used to specify memory and cores of your hardware to get better performance. _\<node N\>_ specifies the [numa](https://en.wikipedia.org/wiki/Non-uniform_memory_access) node id (e.g., 0 to use the memory from the first numa node). _\<physical cores list\>_ specifies phsysical cores which you are using from the _\<node N\>_ numa node. You can use [_lscpu_](https://man7.org/linux/man-pages/man1/lscpu.1.html) command in Linux to check the numa node information.

(2) For all quantization benchmarks, both quantization and inference stages will be triggered by default. For quantization stage, it will auto-generate the quantized model named "best_model.pt" in the "--output-dir" path, and for inference stage, it will launch the inference with the quantized model "best_model.pt".  For inference-only benchmarks (avoid the repeating quantization stage), you can also reuse these quantized models for by adding "--quantized-model-path <output_dir + "best_model.pt">" .


## Miscellaneous Tips
Intel® Extension for PyTorch\* also provides dedicated optimization for many other Large Language Models (LLM), which cover a set of data types that are supported for various scenarios. For more details, please check this [Intel® Extension for PyTorch\* doc](https://github.com/intel/intel-extension-for-pytorch/blob/release/2.4/README.md).
