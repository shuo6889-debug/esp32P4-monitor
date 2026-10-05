# annotated/ 目录说明（先读我）

这个目录是 **ESP32-P4 手机监控**工程（`D:\espidf\jianshi`）的**带注释阅读副本**。

**原始工程一个字节都没有改动。** 这里只是把同样的代码抄了一份，逐段加上中文注释，
方便你一边读一边理解「为什么这么写」。

---

## 从哪里开始读

| 顺序 | 文件 | 是什么 |
|---|---|---|
| **1** | [00-项目总结与学习路线.md](00-项目总结与学习路线.md) | **先读这个**。项目全解 + 数据流水线 + 从零学会写它的分阶段路线 + 排错手册 |
| **2** | [main/main.cpp](main/main.cpp) | 主角：全部应用逻辑（375 行，含逐段注释） |
| **3** | [main/example_config.h](main/example_config.h) | 硬件参数：SCCB 引脚、lane 速率、摄像头格式名 |
| **4** | [main/CMakeLists.txt](main/CMakeLists.txt) | 组件与依赖的声明方式 |
| **5** | [main/idf_component.yml](main/idf_component.yml) | 第三方组件：esp_hosted / esp_wifi_remote / sensor_init |
| **6** | [sdkconfig.defaults](sdkconfig.defaults) | 芯片级开关：PSRAM 200MHz、16MB Flash、FreeRTOS tick |
| **7** | [CMakeLists.txt](CMakeLists.txt) | 工程构建入口 |
| **8** | [.vscode/settings.json](.vscode/settings.json)、[.vscode/launch.json](.vscode/launch.json) | 本机环境与调试配置 |
| **9** | [README.md](README.md) | 原工程自带的使用说明（逐段注解版） |
| **10** | [.clangd](.clangd) | clangd 语言服务器配置：为什么必须丢掉 `-f*` / `-m*` 参数 |

> **关于没有加注释的文件**（都是「看了也没用」的样板，不是遗漏）：
> - `.devcontainer/` —— 官方「ESP-IDF QEMU」模板，与本工程的实际硬件（真实摄像头 + 真实 Wi-Fi）
>   无关，没有任何定制内容，建议直接忽略。
> - `.gitignore` —— 官方模板的忽略规则清单，无项目特有逻辑。唯一值得记住的是它保留了
>   `managed_components/`（第三方源码不入库）和忽略了 `sdkconfig`（自动生成的配置快照不入库），
>   这两条是 ESP-IDF 工程的标准做法。
> - `sdkconfig`、`dependencies.lock`、`build/` —— 全部是自动生成的产物，不应该手改，也不适合加注释。

---

## 注释约定

- **所有中文注释都是本次新增的**；原文件自带的英文注释**原样保留**，需要展开时后面会紧跟「中文补充：」。
- `① ② ③` 表示**执行顺序**。
- `★` 和「极易漏掉的一步」标记的是**最容易出错的地方**，重点看。

---

## 代码一致性怎么保证

注释副本里的**代码与原工程逐字一致**，这一点由脚本校验，不是口头保证。

```powershell
# 用 Python 3 运行（Windows 上如果 python 不在 PATH，可用完整路径）
python D:\espidf\jianshi\annotated\check-code-identical.py
```

脚本做的事：把原文件和副本都按同样规则**剔除注释**，再逐行比对（忽略行首尾空白与空行）。
所以它比较的是**纯代码**——如果全部输出 `[OK]`，就说明副本可以放心当作原代码阅读。

本次校验结果（10/10 通过）：

```
[OK]   main/main.cpp               代码一致（原 337 行）
[OK]   main/example_config.h       代码一致（原 5 行）
[OK]   main/CMakeLists.txt         代码一致（原 5 行）
[OK]   main/idf_component.yml      代码一致（原 13 行）
[OK]   CMakeLists.txt              代码一致（原 3 行）
[OK]   sdkconfig.defaults          代码一致（原 10 行）
[OK]   .clangd                     代码一致（原 2 行）
[OK]   .vscode/settings.json       代码一致（原 37 行）
[OK]   .vscode/launch.json         代码一致（原 10 行）
[OK]   README.md                   代码一致（原 16 行）
```

> 若在 Windows 终端里看到中文变成乱码，只是控制台代码页问题，
> 不影响校验结果（退出码 0 即全部通过）。

---

## 重要：不要在这里编译

本目录只是**阅读材料**，缺少 `build/`、`sdkconfig`、`managed_components/` 等构建所需内容，
在这里执行 `idf.py build` 会失败。

要编译、烧录、改代码，回到原始工程：

```powershell
cd D:\espidf\jianshi
idf.py build
idf.py -p COM5 flash monitor
```

部署前记得先改 `main/main.cpp` 里的 `kApPassword`（当前是示例密码 `p4camera8`）。
