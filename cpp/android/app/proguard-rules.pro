# 保持 JNI 入口类与方法名，避免 R8 混淆。
-keep class com.formatconverter.app.NativeCore { *; }
-keepclasseswithmembernames class * { native <methods>; }