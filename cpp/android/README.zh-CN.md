# 安卓版（v1.6.0 · 已在雷电模拟器实测）

这是 C++ 核心的 Android 外壳，复用同一套 `cpp/` 转换代码：

- JNI：`cpp/android/fc_jni.cpp`（流式事件 + 取消 + 图片解码桥）
- Java 界面：`cpp/android/app/src/main/java/com/formatconverter/app/`
  - `MainActivity.java` 队列 / 进度 / 取消 / 分享 / 自检
  - `NativeCore.java` JNI 绑定
  - `ImageCodec.java` 系统图片解码器桥（HEIC / AVIF 等）
  - `OutputPublisher.java` 发布到系统媒体库（相册 / 下载）
- 原生构建：根目录 `cpp/CMakeLists.txt`（`ANDROID` 分支会生成 `libfccore_jni.so`）
- 图片解码：`cpp/src/image_codec_android.cpp`（通过 JNI 调 BitmapFactory / ImageDecoder）

## 当前支持

不依赖任何外部二进制，完全离线：

- 图片格式互转（输出 PNG / JPEG / WebP / BMP / GIF / ICO / TGA / PPM）
- 输入图片额外支持 **HEIC / HEIF / AVIF**（系统 `ImageDecoder`）与 **TIFF**（内置解码器，
  未压缩 / LZW / PackBits / Deflate，支持 RGB / 灰度 / 调色板）
- 图片转 PDF、多图合并 PDF、多图合成动图 GIF
- PDF 转图片 / 文字 / Word / Excel / PPT、合并 / 拆分 / 旋转 / 压缩（PDFium）
- JSON / YAML / XML / XLSX / CSV 互转、Excel/CSV 转 JSON
- **Word 转纯文本**：内置 docx 文本提取（miniz + tinyxml2），不需要 LibreOffice
- **音视频**：
  - 提取音轨（AAC 无损封装为 M4A，不解码）
  - 视频转 GIF（抽帧 + 核心合成）
  - 视频转 MP4（H.264，走 Media3 Transformer 真转码：解码 + 重编码）
  - 音频转 M4A（AAC，MP3 等转成 AAC）
  - 音频转 MP3（MediaCodec 解码 + shine 编码，LGPL-2.0；支持 32/44.1/48 kHz、单/双声道）
  真转码用官方 `androidx.media3:media3-transformer`（Apache-2.0），没有引入 FFmpeg，
  APK 只多了约 2 MB；能力边界由系统 MediaCodec 决定（MP3 编码、WMV/FLV 容器不支持）。
- 图片质量（1–100）、PDF 渲染 DPI（100/150/200/300）可调
- **入库与分享**：图片进 `Pictures/格式转换工具`，其它文件进 `Download/格式转换工具`，
  无需存储权限（Android 10+ 走 MediaStore），结果可直接分享
- **接收分享**：相册/文件管理器里「分享到」本应用或「用本应用打开」图片/PDF
- **前台服务**：转换期间常驻通知栏显示进度，切到后台不会被系统回收
- **崩溃兜底**：未捕获异常写 `files/last_crash.txt`，关于页可查看/导出
- **中英双语**、深色模式、横屏布局、自适应图标
- **设备自检**：一个按钮跑通图片、解码桥、PDF、数据共 18 条链路

Office 互转依赖 LibreOffice，音视频依赖 FFmpeg，Android 上暂未集成；
后续可接入 FFmpeg 库（或 MediaCodec）与 Collabora/ONLYOFFICE，或改为服务端转换。

## 界面（Material 3）

2026-11-09 把界面从「系统默认控件 + 四个下拉框」重做成 Material 3：

- 依赖 `com.google.android.material:material:1.12.0` + `androidx.appcompat:appcompat:1.7.0`；
  `MainActivity` 继承 `AppCompatActivity`，主题用 `Theme.Material3.DayNight.NoActionBar`。
- **三级胶囊选择**代替 Spinner：类别（图片 / PDF / 数据）→ 转换方式 → 目标格式。
  类别用浅底色胶囊做层级区分，转换方式和目标格式用主色填充；长 label 自动去掉括号说明。
- 卡片式分区：顶部标题 + 版本标签，中间「文件队列」卡片（格式角标 + 文件名 + 状态），
  底部固定操作卡片（进度、开始 / 取消、保存开关、输出目录）。
- 队列项按状态换色：等待中=琥珀、完成=绿色、失败=红色（文字 + 角标底色）。
- 日志面板默认收起，按需展开，高度随内容自适应（最多 140dp），出错时自动展开。
- 深色模式：`values-night/colors.xml` 提供整套配色，跟随系统（主题为 DayNight）。
- 自适应图标：`mipmap-anydpi-v26/ic_launcher.xml` + 矢量前景 / 背景，桌面显示蓝底双向箭头。
- `launchMode="singleTask"`：从相册多次「分享到」本应用时，文件会累加到同一个队列，
  不会开出一堆实例。

## 构建步骤

1. 安装 Android Studio（JDK 17、Android SDK 34、NDK 27.2.12479018、
   SDK CMake 3.22.1）。项目当前 `compileSdk = 34`、`targetSdk = 34`。
2. **工程路径必须是纯 ASCII**。AGP 会直接拒绝非 ASCII 路径，NDK 的 clang 也会报
   `unable to open output file: Permission denied`。项目本身放在中文目录时，
   用目录联接映射一个 ASCII 路径即可（不需要复制源码）：

   ```powershell
   cmd /c mklink /J C:\fcws "D:\code\format-converter"
   # 然后在 C:\fcws\cpp\android 下构建
   ```

   实测：源码在中文路径、通过 `C:\fcws` 联接构建可以正常产出 APK。
3. 准备 PDFium Android 预编译包（两个 ABI）：

   ```text
   cpp/third_party/pdfium-android-arm64-v8a/
   cpp/third_party/pdfium-android-x86_64/
   ```

   下载：<https://github.com/bblanchon/pdfium-binaries/releases>
   （选择 `pdfium-android-arm64.tgz`、`pdfium-android-x64.tgz`）。

4. 复制 PDFium 到 jniLibs：

   ```powershell
   cd cpp/android
   powershell -ExecutionPolicy Bypass -File .\sync_pdfium.ps1
   ```

5. 用 Android Studio 打开 `cpp/android`，等待 Gradle 同步，然后运行 `app`。

   命令行构建（需要 Gradle 8.x + JDK 17）：

   ```powershell
   $env:JAVA_HOME='C:\Program Files\Android\Android Studio\jbr'   # 或自己的 JDK 17
   $env:ANDROID_HOME="$env:LOCALAPPDATA\Android\Sdk"
   $env:ANDROID_NDK_PATH="$env:ANDROID_HOME\ndk\27.2.12479018"
   cd C:\fcws\cpp\android
   gradle :app:assembleDebug
   ```

   仓库也带了 `gradlew`；如果它下载 Gradle 发行版时出现证书错误，
   直接用 Android Studio 内置 Gradle 或本机 `gradle` 即可。

APK 输出（按 ABI 拆分）：

```text
cpp/android/app/build/outputs/apk/debug/app-arm64-v8a-debug.apk   # 真机
cpp/android/app/build/outputs/apk/debug/app-x86_64-debug.apk      # 模拟器
cpp/android/app/build/outputs/apk/debug/app-universal-debug.apk   # 通用包
```

签名 release 需要一个本地 keystore（不入库）：

```powershell
keytool -genkeypair -keystore fc-release.jks -alias formatconverter `
  -keyalg RSA -keysize 2048 -validity 10000 -storepass <pwd> -keypass <pwd> `
  -dname "CN=FormatConverter"
# 工程根目录写 keystore.properties：storeFile / storePassword / keyAlias / keyPassword
gradle :app:assembleRelease
```

单元测试：`gradle :app:testDebugUnitTest`（JVM 侧，验证 MIME 映射等纯逻辑）。

## 实现要点

- 输入文件通过系统文件选择器（SAF）复制到应用缓存目录；输出先写入
  `Android/data/com.formatconverter.app/files/output/`，再发布到媒体库。
- JNI 事件是流式的：C++ 每完成一个文件就回调 Java，界面实时刷新进度。
- `NativeCore.cancel()` 会置取消位并调用 `fc_context_cancel()`，
  C++ 引擎在文件之间检查，已开始的文件会跑完。
- 界面记住上次的转换方式与目标格式（SharedPreferences），长按队列条目可删除。
- 图片解码桥通过 `JNI_OnLoad` 缓存 JavaVM 和 `ImageCodec` 方法；
  解码结果按 RGBA8 交给核心，EXIF 方向仍由核心统一处理。
- 移动端内存有限：`engine.cpp` 在 Android 上把并发解码限制为 3，
  `ImageCodec` 对超过 2400 万像素的图会按 2 的幂降采样。

## 已验证（雷电模拟器 14，Android 14 x86_64）

- 安装启动、原生库加载、注册表读取正常；
- 自检 17 条链路全部通过（图片 / 解码桥 / PDF 全家桶 / 数据互转）；
- SAF 选文件 → 转换 → 入库 → 分享可用；
- 批量 6 个 3600×2400 图片，1.2 秒后点取消：已完成 3 个、其余跳过，日志标记「已取消」；
- 中文文件名（含空格）从选择器、分享入口到输出全程正常。