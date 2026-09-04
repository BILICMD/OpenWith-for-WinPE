# OpenWith for WinPE

一个用于WinPE的“打开方式”程序，替换 `System32\OpenWith.exe` 后，为未关联文件提供应用选择窗口。

## 功能

- 提供32位和64位版本。
- 从系统的“打开方式”列表读取应用，支持“始终”和“仅一次”。
- WinUI3 样式，高仿原版 Fluent Design
- 全局G2连续圆角曲线+自绘柔和阴影
- 自动检测OS版本切换窗口&元素圆角/直角
- 自动读取SPI_GETDROPSHADOW API启用/关闭窗口阴影
- 最低兼容至 Windows 7（Vista 未经测试，XP 不支持）
- 本体大小仅占200kb左右

## 文件说明

```text
OpenWith_x64.exe       x64 发布文件
OpenWith_x86.exe       x86 发布文件
src/                   C 源码与资源脚本
src/resources/         图标和清单文件
```

## 关联方式:

将对应架构的程序复制为：

```text
%SystemRoot%\System32\OpenWith.exe
```

在 `PECMD.INI` 中清除旧的 `OpenWithLauncher` 和 `ImmersiveBroker` 入口，再将下列命令指向这个程序：

```text
HKCR\Undecided\shell\open\command
HKCR\Unknown\shell\Open\command
HKCR\Unknown\shell\openas\command
HKCR\Unknown\shell\OpenWithSetDefaultOn\command
```

双击未关联文件：
```text
HKCR\Unknown\shell\Open\command
HKCR\Unknown\shell\openas\command
HKCR\Undecided\shell\open\command
```

以上命令最终都直接执行：
```text
%SystemRoot%\System32\OpenWith.exe "%1"
```

PECMD.INI 中必须删除系统的 COM/Immersive 启动入口，否则属性页按钮会等待系统 OpenWith COM 服务器：
```text
EXEC -wait -hide reg.exe delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\OpenWith" /v OpenWithLauncher /f
EXEC -wait -hide reg.exe delete "HKLM\SOFTWARE\Classes\Applications\OpenWith.exe" /v ImmersiveBroker /f
```

并为 OpenWithSetDefaultOn 建立直接调用：
```text
EXEC -wait -hide reg.exe add "HKCR\Unknown\shell\OpenWithSetDefaultOn" /v ProgrammaticAccessOnly /t REG_SZ /f
EXEC -wait -hide reg.exe add "HKCR\Unknown\shell\OpenWithSetDefaultOn" /v MultiSelectModel /t REG_SZ /d Single /f
EXEC -wait -hide reg.exe add "HKCR\Unknown\shell\OpenWithSetDefaultOn\command" /ve /t REG_EXPAND_SZ /d "%SystemRoot%\System32\OpenWith.exe \"%%1\"" /f
```

PECMD 配置中的 %%1 最终写入注册表后是 %1，代表所选文件的完整路径


命令格式：

```text
%SystemRoot%\System32\OpenWith.exe "%1"
```


## 许可证

本项目采用 [GNU GPL v3.0 或更高版本](LICENSE) 发布。

## 署名

基于本项目修改、编译、整合和二次发布时，请表明原作者及项目出处
