import org.gradle.api.tasks.compile.JavaCompile
import java.util.Properties

plugins {
    id("com.android.application")
}

val ndkOverride = System.getenv("ANDROID_NDK_PATH")

android {
    namespace = "com.formatconverter.app"
    compileSdk = 34
    // 可用 ANDROID_NDK_PATH 指定 ASCII 路径，避免 NDK clang 在中文路径下执行失败。
    ndkVersion = "27.2.12479018"
    if (!ndkOverride.isNullOrBlank()) {
        ndkPath = ndkOverride
    }

    defaultConfig {
        applicationId = "com.formatconverter.app"
        minSdk = 26
        targetSdk = 34
        versionCode = 4
        versionName = "1.7.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DFC_BUILD_CLI=OFF",
                    "-DFC_BUILD_SHARED=OFF",
                    "-DFC_BUILD_TESTS=OFF"
                )
                cppFlags += "-std=c++17"
            }
        }
    }

    // 按 ABI 拆分：真机装 arm64-v8a 包、模拟器装 x86_64 包，体积各减一半；
    // universal 包仍然产出，方便随手安装/回退。
    splits {
        abi {
            isEnable = true
            reset()
            include("arm64-v8a", "x86_64")
            isUniversalApk = true
        }
    }

    signingConfigs {
        create("release") {
            val propsFile = rootProject.file("keystore.properties")
            if (propsFile.exists()) {
                val props = Properties()
                propsFile.inputStream().use { props.load(it) }
                storeFile = rootProject.file(props.getProperty("storeFile"))
                storePassword = props.getProperty("storePassword")
                keyAlias = props.getProperty("keyAlias")
                keyPassword = props.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (rootProject.file("keystore.properties").exists()) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    externalNativeBuild {
        cmake {
            // 兼容两种目录布局：
            //   源码布局     cpp/android/app  →  ../../CMakeLists.txt
            //   平级构建布局 xxx/android/app  →  ../../cpp/CMakeLists.txt
            val siblingLayout = file("../../cpp/CMakeLists.txt")
            path = if (siblingLayout.exists()) siblingLayout else file("../../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    // Material 3 组件（Chip / Card / Button / Slider）与 AppCompat 基类。
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.appcompat:appcompat:1.7.0")

    // 音视频转码：官方 Media3 Transformer（Apache-2.0，纯 Java 走系统 MediaCodec）。
    implementation("androidx.media3:media3-transformer:1.4.1")
    implementation("androidx.media3:media3-effect:1.4.1")
    implementation("androidx.media3:media3-common:1.4.1")

    // 纯 JVM 单元测试（./gradlew :app:testDebugUnitTest）。
    testImplementation("junit:junit:4.13.2")
}

// 源码含中文，固定用 UTF-8 编译，避免 Windows 默认编码（GBK）把字符串和日志弄乱。
tasks.withType<JavaCompile>().configureEach {
    options.encoding = "UTF-8"
}