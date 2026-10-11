# attic — 标定时代的一次性脚本

这些脚本是一次性标定/实验产物，结论已固化进 README §13 与各 preset
（`src/scene_presets.h`），不再维护。保留供考古：想知道某个标定值
"当时怎么来的"就看这里。

| 脚本 | 当时用途 |
|------|----------|
| `calibrate_daysky.sh` / `calibrate_daysky2.sh` / `calibrate_daysky2b.sh` | 白天天空标定的三轮参数扫描（§13.6，最终配置进 `bistro` preset 与 `render_bistro_official_match.sh`） |
| `calibrate_fit.py` | 单会话坐标下降自动标定 7 个渲染期参数（README §13.13 P3） |
| `denoise_lab.py` | 降噪器权重实验台（离线迭代用，正式验收走 `run_ball_tests.sh`） |
| `grade_lab.py` | LUT 调色分级实验（生产拟合工具是 `../make_lut_grade.py`） |
| `demod_bisect.py` | **NL-means 完整权重的 Python 复现**（albedo 解调实验——结论：失败，见 README §8；调试降噪器时仍有参考价值） |

运行方式不变（`python3 denoise_lab.py` 之类），但它们假定的工作目录、
env 与标定基线是 2026-09 时的状态，结果不保证可复现。
