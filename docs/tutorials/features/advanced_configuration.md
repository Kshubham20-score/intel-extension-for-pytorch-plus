Advanced Configuration
======================

The default settings for Intel® Extension for PyTorch\* are sufficient for most use cases. However, if users want to customize Intel® Extension for PyTorch\*, advanced configuration is available at build time and runtime. 

## Build Time Configuration

The following build options are supported by Intel® Extension for PyTorch\*. Users who install Intel® Extension for PyTorch\* via source compilation could override the default configuration by explicitly setting a build option ON or OFF, and then build. 

| **Build Option** | **Default<br>Value** | **Description** |

For above build options which can be configured to ON or OFF, users can configure them to 1 or 0 also, while ON equals to 1 and OFF equals to 0.

## Runtime Configuration

The following launch options are supported in Intel® Extension for PyTorch\*. Users who execute AI models on XPU could override the default configuration by explicitly setting the option value at runtime using environment variables, and then launch the execution.

| **Launch Option<br>CPU, GPU** | **Default<br>Value** | **Description** |

| **Launch Option<br>GPU ONLY** | **Default<br>Value** | **Description** |

| **Launch Option<br>Experimental** | **Default<br>Value** | **Description** |

| **Distributed Option<br>GPU ONLY** | **Default<br>Value** | **Description** |
| ------ | ------ | ------ |
| TORCH_LLM_ALLREDUCE | 0 | This is a prototype feature to provide better scale-up performance by enabling optimized collective algorithms in oneCCL and asynchronous execution in torch-ccl. This feature requires XeLink enabled for cross-cards communication. By default, this feature is not enabled with setting 0. |
| CCL_BLOCKING_WAIT | 0 | This is a prototype feature to control over whether collectives execution on XPU is host blocking or non-blocking. By default, setting 0 enables blocking behavior. |
| CCL_SAME_STREAM | 0 | This is a prototype feature to allow using a computation stream as communication stream to minimize overhead for streams synchronization. By default, setting 0 uses separate streams for communication. |

For above launch options which can be configured to 1 or 0, users can configure them to ON or OFF also, while ON equals to 1 and OFF equals to 0.

Examples to configure the launch options:</br>

- Set one or more options before running the model

```bash
export IPEX_LOG_LEVEL=1
export IPEX_FP32_MATH_MODE=TF32
...
python ResNet50.py
```
- Set one option when running the model

```bash
IPEX_LOG_LEVEL=1 python ResNet50.py
```

- Set more than one options when running the model

```bash
IPEX_LOG_LEVEL=1 IPEX_FP32_MATH_MODE=TF32 python ResNet50.py
```
